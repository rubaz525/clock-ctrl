/*  

 Copyright (C) 2016-2018 Michael Boich

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
*/
#include "gps.h"
#include <device.h>
#include <stdlib.h>
#include <stdio.h>
#include "UART_1.h"
#include "RTC_1.h"
#include "prefs.h"
#include "time.h"


#include "ds3231.h"

#define NEWLINE 0x0A
#define CR 0x0D


// handy constants:
uint8 days_in_month[2][12] = {{31,28,31,30,31,30,31,31,30,31,30,31},\
                            {31,29,31,30,31,30,31,31,30,31,30,31}};

int isNumber(double x) 
{
    // This looks like it should always be true, 
    // but it's false if x is a NaN.
    return (x == x); 
}
int isInBounds(double x) 
    {
        return (x <= 180.0 && x >= -180.0); 
    }
    
    
char *field_n(uint8 n, char *sentence){
    char *c = sentence;
    while(n && *c){
        if(*c++ == ','){
            n--;
        }
    }
    if(n) return (0);  // we didn't get to field # n
    else return(c);  // c just advanced past the nth comma
}

// convert two successive decimal digits to an int:
int a_to_int(char *s){
    return 10*(s[0] - 0x30) + (s[1] - 0x30);
}
// utility routines, mostly for the sunrise/sunset calcs:
  char test_str[] = "$GPRMC,123519,A,4807.038,S,01131.000,E,022.4,084.4,230394,003.1,E*6A";

#define is_digit(c) (c >= '0' && c <= '9')
#define Success 1
#define Fail 0

#include <stdio.h>


#define Success 1
#define Fail 0


int coord_available(char *c_ptr){
  // confirms that c_ptr points to a valid latitude or longitude in the NMEA format
  // prevents a crash when the routine is called and the GPS hasn't established a fix yet
  //  Coords are of the form (d{3}|d{5})\.d+(N|S|E|W)
  //  This isn't as precise as it could be, but should serve my needs
    
  int int_digits = 0;
  while(int_digits <=5 && *(c_ptr+2)!='.'){
    // scan to the decimal point, or until we have too many digits
    c_ptr+=1;
    int_digits += 1;
  }
  c_ptr += 2;  // skip over the two integer digits of minutes
  if(int_digits < 1 || int_digits>3){
    return(Fail);  // wrong number of int digits
  }
  if(*c_ptr != '.'){

    return(Fail);
   }
  
  c_ptr += 1;
  while(is_digit(*c_ptr)){
    c_ptr += 1;
  }

  return(Success);
}
float get_lat_or_long(int select){
    float result=0.0;
    char lat_or_long_str[32];
    char *src_ptr;
    char *dst_ptr = lat_or_long_str; 
    if(!sentence_avail) return select == 0 ? 37.368832 : 122.036346;
    src_ptr = field_n(select==0 ? 3 : 5,sentence);
    while(*src_ptr != ','){
        *dst_ptr++ = *src_ptr++;
    }
    *dst_ptr++ = 0;   //terminate string
    
    // parse out the integer degree portion:
    // there are always two integer minutes digits, then a decimal place, 
    // so everything two or more chars before the decimal point is part of the integer degree representation
    src_ptr = lat_or_long_str;
    
    // interim check that the lat/long value is safe to parse:
    if(!coord_available(src_ptr)){
        return select == 0 ? 37.368832 : 122.036346;
    }
    while(*(src_ptr+2) != '.'){
        result = 10*result + (*src_ptr - '0');
        src_ptr++;
    }
    // src_ptr+2 now points to the decimal point. src_ptr points to the first digit of minutes.
    // The minutes string is zero-terminated, so we can just use sscanf to get the value of the minutes
    float minutes;
    sscanf(src_ptr,"%f",&minutes);
    result += minutes/60.0;
    
    // now we have to look at the N/S designator (for latitude) or E/W for Longitude, and adjust accordingly:
    if(select == 0 && (*(field_n(4,sentence)) == 'S')){  // latitude case
        result = -result;
    }
    else if(select==1 && (*(field_n(6,sentence)) == 'E')){
        result = -result;
    }
    
    // shameful hack:  if we didn't get a good number, fake it for now
    if(!isNumber(result) || !isInBounds(result)){
    //if(1 || !isInBounds(result)){
        result = select == 0 ? 37.368832 : 122.036346;
    }
    return result;
}

time_t rmc_sentence_to_unix_time(char *sentence){
    struct tm tm_gps;
    tm_gps.tm_hour = a_to_int(field_n(1,sentence));
    tm_gps.tm_min = a_to_int(field_n(1,sentence)+2);
    tm_gps.tm_sec = a_to_int(field_n(1,sentence)+4); 
   
    // if we reach this point, we need to set the time:
    tm_gps.tm_mday = a_to_int(field_n(9,sentence));
    tm_gps.tm_mon = a_to_int(field_n(9,sentence)+2)-1;  //  map 1..12 to 0..11
    tm_gps.tm_year = 100 + a_to_int(field_n(9,sentence)+4);
    tm_gps.tm_isdst = 0;
    time_t gps_time = mktime(&tm_gps);
    return gps_time;
}

void send_command(char *s){
    uint8 checksum = 0;
    char checksum_as_hex[8];
    UART_1_PutChar(*s++);  // send the $ character
    while(*s){
        UART_1_PutChar(*s);
        checksum = checksum ^ *s++;
    }
    UART_1_PutChar('*');
    sprintf(checksum_as_hex,"%02x",checksum);
    UART_1_PutChar(checksum_as_hex[0]);
    UART_1_PutChar(checksum_as_hex[1]);
    UART_1_PutChar(CR);
    UART_1_PutChar(NEWLINE);
}

void init_gps(){
    strcpy(sentence,"Uninitialized");  // sentence will hold NMEA sentences when they arrive
    sentence_avail = 0;
    UART_1_Start();  
    CyDelay(100);  // for luck
     //send_command("$PSRF100,1,38400,8,1,0");
     send_command("$PSRF103,00,00,00,01");

     send_command("$PSRF103,01,00,00,01");
     send_command("$PSRF103,02,00,00,01");
     send_command("$PSRF103,03,00,00,01");
    
     send_command("$PSRF103,04,00,01,01");
     send_command("$PSRF103,04,01,01,01");      // query this once to start the RTC

    send_command("$PSRF103,05,00,00,01");
     send_command("$PSRF103,06,00,00,01");
//send_command("$PSRF103,00,01,00,01");
    //hard_command();
    //RTC_1_WriteIntervalMask(RTC_1_INTERVAL_SEC_MASK);
    RTC_1_WriteIntervalMask(RTC_1_INTERVAL_SEC_MASK | RTC_1_INTERVAL_MIN_MASK);
    RTC_1_Start();
    RTC_1_Stop();
}
typedef enum {awaiting_dollar,awaiting_char, in_sentence} gps_parse_state;
extern int second_has_elapsed;


float gps_long,gps_lat;


// Sets the RTC to equal the incoming GPS Time, EXCEPT, if the incoming time is just one second
// greater than the current RTC time, we'll optimistically assume that the onePPS interrupt will arrive 
// in the next second, and set the time for us.
// because I'm lazy, I only check this when decrementing the incoming second count doesn't cause an underflow
// 
void set_rtc_to_gps(){
    static int seed = 0;
    static int time_set=0;
    
    struct tm tm_gps;

    time_t gps_time = rmc_sentence_to_unix_time(sentence);
    
    RTC_1_TIME_DATE *psoc_rtc = RTC_1_ReadTime();
    time_t psoc_time = psoc_to_unix(psoc_rtc);
    
    // no need to set if the ds3231 is just one second behind the gps RMC sentence:
//    if(gps_time - psoc_time == 1){
//        return;
//    }
    if(1){
        setDS3231(gps_time);
        unix_to_psoc(gps_time, psoc_rtc);
        RTC_1_Init();
    }
        
    // create a seed for the random number generator based on time and date:
    if(seed==0){
        seed = (int) gps_time;
        srand(seed);
    }
    time_set=1;
    
}

void consume_char(char c){
    static gps_parse_state state = awaiting_char;
    char expected_char[] = "$GPRMC";
    //char expected_char[] = "$GPGGA";
    extern int pps_available;
    
    static uint8 index=0;
    static uint8 buf_index=0;
    
    switch(state){
        case awaiting_dollar:
          if(c=='$'){
            state = awaiting_char;
            index=1;
            sentence[buf_index++]=c;
            }
          break;
            
        case  awaiting_char:
            if(c == expected_char[index]){
              index += 1;
              sentence[buf_index++] = c;
              if(expected_char[index]==0){  // we've found all the expected_char characters
                state = in_sentence;
              }      
            }
            else {  // we have to start over
                state = awaiting_dollar;
                index=0;
                buf_index=0;
            }
           break;
            
            case in_sentence:
              sentence[buf_index++] = c;
              if(c==NEWLINE){
                sentence_avail=1;
                sentence[buf_index++] = 0;  //terminate the string
                index=0;
                buf_index=0;
                state = awaiting_dollar;
                
               // second_has_elapsed = 1;
               if(global_prefs.prefs_data.use_gps && pps_available){
                  set_rtc_to_gps();
               }
               //scrape_lat_long();
            }
            break;
    }
}
void offset_time(RTC_1_TIME_DATE *t, int hours){
    int h = t->Hour + hours;
    if(h>23){
     h -= 24;
     increment_date(t,1);
    }
    else{
     if(h < 0){
       h += 24;
       increment_date(t,-1);
      }
    }
    t->Hour = h;
}

int is_leap_year(int y){
 if(4*(y/4) != y) return 0;
 // divisible by 4.  if not visible by 100, it's a leap year:
 if(!(100*(y/100)==y)) return(1);

// divisible by 4 and by 100.  Leap year if divisible by 400:
 if(y == 400*(y/400)) return 1;
 else return(0);
}
void increment_date(RTC_1_TIME_DATE *t,int incr){
    int d,m,y,dayOfWeek;
    int year_type = is_leap_year(y);  // 0 for regular, 1 for leap
    d = t->DayOfMonth;
    m = t->Month;
    y = t->Year;
    dayOfWeek = t->DayOfWeek;
    
    d += incr;  // increment or decrement the date incr should be +1 or -1 only
    if(d > days_in_month[year_type][m-1]){
        m +=1;
        d=1;
        if(m>12){  // we went into the new year
            m=1;
            y+=1;
        }
    }
    
    if(d==0){
        m-=1;  // go to prev month
        if(m<1){  // if month goes to zero, wrap to December 31 of prior year:
         m+=12;
         y-=1;
         d=31;  // i.e. 12/31/prev-year
        }
       else d = days_in_month[year_type][m-1];  //wrap to last day of previous month
    }
    t->DayOfMonth=d;
    t->Month=m;
    t->Year = y;
    
    
}
// since much of the pps logic is at the interrupt level, globals are used to share info.
// We try to keep the manipulation of those globals contained in a couple of functions:
void invalidate_gps_pps(){
    extern int pps_available;
    extern int pulse_count;
    pps_available = 0;
    pulse_count=0;
}

// testing to see if fields are present in tm struct:
void test(){
    struct tm tm_test={.tm_mon=11, .tm_isdst=1};
}
/* [] END OF FILE */

# Timer driver for zybo z7-10
Timer driver for Zybo z7-10 board with alarm application.

## Alarm application
To use application use make run. To read state of timer (current time) press *Enter*
Functions for buttons:
- First button Start/Stop timer
- Second button decrement 10 sec timer
- Third button increment 10 sec timer
- Fourth button exits application

## Driver
To communicate with driver, after make, run *insmod timer_driver.ko* to insert in */dev/* path
Communication with driver module:
- writing into driver module in this format m,n,e where:
  - m: mode of execution p|s or P|S (pause and start)
  - n: number of miliseconds for setup
  - e: enable bit for initial value.
- reading from driver is done with standard read functions
  - return value: in nanoseconds
  

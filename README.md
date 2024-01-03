# Purpose of this repo:
This is a place for my ch32v003fun related mini-projects. It's mostly a place for me to test out a very small, well defined problem
to either later contribute to the ch32v003fun repo or to test out an idea that might get integrated into some of my "work" projects.
Most of my PoC code here is rough, unsafe and probably very buggy and should be used with a healthy amount of suspicion.

# Install
This repo assumes that the main repo: ch32v003fun got cloned side-by-side to itself in order to access its toolchain via symbolic links.

## Clone
`git clone https://github.com/cnlohr/ch32v003fun.git`  
`git clone https://github.com/DeadBugEngineering/ch32v003fun_shenanigans.git`  
or specify your own forks  

## Symlink
To compile and upload, we need access to the ch32v003fun directory through symbolic links.  

### Linux / MacOs:  
```
cd ch32v003fun_shenanigans
ln -s ../ch32v003fun/ch32v003fun ch32v003fun
ln -s ../ch32v003fun/extralibs extralibs
ln -s ../ch32v003fun/minichlink minichlink
ln -s ../ch32v003fun/examples examples
```

### References
Thanks to the symlinks, we can run make directly from within `ch32v003fun_shenanigans/`.  
Thus, we can use these path references:  
for ch32v003fun_shenanigans lib: `../lib`, which is a direct relative path  
for extralibs: `../extralibs`, which is a relative path through the symlink  

# The Mini-projects:

## HiVoPuCounter: 
This module is designed to log the information of a (here) Xenon-flashbulb that can be used to approximate the
remaining life-time of it to be able to get good use out of each lamp but also prevents damage to optical components due to a
light-bulb going through an unscheduled disassembly process. The energy of each pulse is dependent on the voltage across the
discharge capacitor (bank), so logging the voltage and the amount of discharges at this same-ish voltage is done by storing
the voltage (here: 400V..3200V) by dividing this voltage range into (here) 100 bins. Each bin stores the amount of discharges
within that voltage binning and stores the number of discharges with uint32 to avoid value overflows in any practical scenario.
The voltages below 400V is not relevant as the lamp will not fire consistently below that voltage (in the target unit). 
The logging data is stored inside an FRAM to have a persistant log of the data. 
The main loop of the logging firmware runs with 1 kHz (here) and samples an analog voltage equivalent to a much higher voltage
across the capacitor (bank). A discharge event is detected by calculating the difference between the current analog sample and
the rolling average of this value. When a certain difference is reached, a discharge event for a specific voltage bin is
incremented by one (by first getting the current value from the FRAM and then write back the incremented value),
and the discharge detection routine gets overridden for a certain amount of time (depends on the application) to prevent
counting multiple discharges in case of an unusually long pulse.
The target flasher design operates at 1Hz repetition rate to keep the thermal stress on the bulbs managable, 
which gives this logger ample time to output diagnostic data over the UART and the optional OLED display between two discharge
events. This detection method only counts the sharp dips in the voltage that occur in an discharge event and will ignore the
slow decline of the voltage level that happens while deenergizing the device.
The output via the OLED display shows the binning voltage and the amount of logged flashes and the UART outputs all bins with
at least one count.

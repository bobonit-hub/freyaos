10 ! PIN, PWM and ADC on the pretend pins of the host build: ports
20 ! A, B and C, PA2 and PA3 the console's, PWM on ports A and B,
30 ! the ADC on PA0-7, PB0-1 and PC0-5.  The errors, which each end
40 ! a run, are in session.in.
50 PRINT PIN("PB5"); PIN("PB5",1); PIN("PB5"); PIN("pb5",0); PIN("B5")
60 PRINT PIN("PB5","TOGGLE"); PIN("PB5","toggle"); PIN("PB5",-1)
70 PRINT PIN("PB0","UP"); PIN("PB0","out",1); PIN("PB0","in")
80 PRINT PIN("PB0","OD","TOGGLE"); PIN("PB0","ANALOG"); PIN("PB0","DOWN")
100 ! a name may be computed, and a level is any number: 0 or not
110 P$ = "PC" + STR$(15)
120 FOR I = 0 TO 3
130   PRINT PIN(P$, I - 2);
140 NEXT I
150 PRINT
160 PRINT PIN("PA1", 3 > 2), PIN("PA1", 3 < 2)
200 ! the analog sources
210 PRINT ADC("PA0"), ADC("pb1"), ADC("C5")
220 PRINT ADC("TEMP"), ADC("temp"), ADC("VREF")
300 ! a channel is started, changed and stopped; the duty may be fractional
310 PRINT PWM("PB6",1000,25); PWM("PB6",2000,7.5); PWM("pb6",50,0); PWM("PB6",50,100)
320 PRINT PWM("PB6"); PWM("PA8",1000000,50); PWM("PA8")
400 ! stopping a channel that is not running is refused
410 PRINT PWM("PB6")
420 PRINT "NOT REACHED"

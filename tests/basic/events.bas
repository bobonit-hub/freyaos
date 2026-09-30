10 ! ON TIMER, ON KEY, TIMER ON/OFF, KEY ON/OFF, DO ... LOOP, INKEY$.
20 ! The key is PA0 of the host's pretend board, pressed by driving
30 ! the pin low; the keys typed are events.in.  The counts depend on
40 ! the machine's timing, so ranges are printed, not values.
50 N = 0: K = 0: I = 0
60 ON TIMER(50) GOSUB 1000
70 TIMER ON
80 ON KEY("PA0") GOSUB 2000
90 KEY("PA0") ON
100 ! the main work: a press per turn, and a tick every 50 ms
110 DO
120   X = PIN("PA0", 0)
130   SLEEP 60
140   X = PIN("PA0", 1)
150   SLEEP 60
160   I = I + 1
170 LOOP UNTIL INKEY$ = "q"
180 PRINT "turns"; I; "presses"; K; "ticks in range"; N >= 5 AND N <= 10
200 ! TIMER OFF stops the ticks, KEY OFF the presses
210 TIMER OFF: M = N
220 KEY("PA0") OFF
230 X = PIN("PA0", 0): SLEEP 100: X = PIN("PA0", 1)
240 PRINT "off"; N = M; K
300 ! a bounce is not a press, a press held for 20 ms is one
310 KEY("PA0") ON
320 FOR J = 1 TO 5: X = PIN("PA0", 0): X = PIN("PA0", 1): NEXT J
330 SLEEP 50
340 PRINT "bounce"; K
350 X = PIN("PA0", 0): SLEEP 50
360 PRINT "press"; K
370 X = PIN("PA0", 1): SLEEP 50
380 PRINT "release"; K
400 ! an active-high key, on a pull-down
410 ON KEY("PB1", 1) GOSUB 2100
420 KEY("PB1") ON
430 X = PIN("PB1", 1): SLEEP 50
440 PRINT "high"; H
500 ! ticks arrive during a long SLEEP, which goes on to its end
510 TIMER ON: M = N: T = TIME
520 SLEEP 300
530 PRINT "slept"; TIME - T >= 290; "ticks"; N - M >= 4 AND N - M <= 7
540 TIMER OFF
600 ! a handler is not entered again while it runs, and one that takes
610 ! longer than the period fires once per return, not per tick
620 ON TIMER(20) GOSUB 1100
630 D = 0: E = 0: P = 0
640 TIMER ON
650 SLEEP 200
660 TIMER OFF
670 PRINT "nested"; E; "handled"; D >= 2 AND D <= 6
700 ! DO ... LOOP in its forms; a jump out and in again
710 I = 0
720 DO WHILE I < 3: I = I + 1: LOOP
730 DO UNTIL I = 0: I = I - 1: LOOP
740 DO: I = I + 2: LOOP WHILE I < 5
750 DO: I = I + 1: LOOP UNTIL I >= 8
760 PRINT I;
770 DO WHILE 0: PRINT "never";: LOOP
780 DO UNTIL 1: PRINT "never";: LOOP
790 DO
800   I = I + 1
810   IF I = 10 THEN 790
820   IF I = 12 THEN 850
830 LOOP
840 PRINT "not reached"
850 PRINT I
860 FOR J = 1 TO 2: DO: J = J + .5: PRINT J;: LOOP UNTIL J > 1.7: NEXT J
870 PRINT
900 ! the keys typed and not taken are still there
910 PRINT "left "; INKEY$; INKEY$; "|"
920 END
1000 N = N + 1
1010 RETURN
1100 P = P + 1
1110 IF P > 1 THEN E = E + 1
1120 SLEEP 45
1130 P = P - 1: D = D + 1
1140 RETURN
2000 K = K + 1
2010 RETURN
2100 H = H + 1
2110 RETURN

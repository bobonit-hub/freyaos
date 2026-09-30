10 ! DATE$, TIME$, TIME and SLEEP.  The clock reads whatever the host
20 ! says, so what is checked is the shape of it and that it moves.
30 D$ = DATE$
40 T$ = TIME$
50 PRINT LEN(D$), LEN(T$)
60 PRINT SEG$(D$,5,5); SEG$(D$,8,8); SEG$(T$,3,3); SEG$(T$,6,6)
100 ! every other character is a digit
110 B = 0
120 FOR I = 1 TO 10
130   IF I = 5 OR I = 8 THEN 160
140   C = ASC(SEG$(D$,I,I))
150   IF C < 48 OR C > 57 THEN B = B + 1
160 NEXT I
170 FOR I = 1 TO 8
180   IF I = 3 OR I = 6 THEN 210
190   C = ASC(SEG$(T$,I,I))
200   IF C < 48 OR C > 57 THEN B = B + 1
210 NEXT I
220 PRINT "BAD DIGITS"; B
300 ! the fields are in range
310 Y = VAL(SEG$(D$,1,4))
320 M = VAL(SEG$(D$,6,7))
330 N = VAL(SEG$(D$,9,10))
340 H = VAL(SEG$(T$,1,2))
350 I = VAL(SEG$(T$,4,5))
360 S = VAL(SEG$(T$,7,8))
370 PRINT Y >= 1970, M >= 1 AND M <= 12, N >= 1 AND N <= 31
380 PRINT H <= 23, I <= 59, S <= 59
400 ! TIME counts milliseconds and never goes back
410 A = TIME
420 PRINT A >= 0
430 SLEEP 100
440 B = TIME
450 PRINT B - A >= 100, B - A < 5000
460 SLEEP 0
470 PRINT TIME >= B
500 ! a negative wait is refused
510 SLEEP -1
520 PRINT "NOT REACHED"

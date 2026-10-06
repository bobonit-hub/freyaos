100 ! The elementary functions and the conversions, over enough
110 ! arguments that every build has to round each operation the same
120 ! way to print the same digits: a fused multiply-add, or a different
130 ! series, changes some of them.
140 FOR I = 1 TO 240
150   X = I * .0731 - 8.77
160   PRINT EXP(X); SIN(X * 3.1); COS(X * 2.3); ATN(X * 1.7)
170 NEXT I
200 FOR I = 1 TO 240
210   X = I * I * .0173 + .00031 * I
220   PRINT LOG(X); SQR(X); X ^ 1.37; LOG10(X)
230 NEXT I
300 FOR I = 1 TO 120
310   X = I * .0517 + .5
320   PRINT TAN(X); X ^ (-2.71); (1 / X) ^ 7; EXP(-X * X)
330 NEXT I
400 ! text to number and back
410 FOR I = 1 TO 120
420   A$ = STR$(I * 7.13E-7) + " " + STR$(I * 31.7E5)
430   PRINT A$; VAL(SEG$(A$, 1, 8)) * 3; VAL("1." + STR$(I) + "E" + STR$(I - 60))
440 NEXT I

10 DEF FNMAX(A,B)
20   IF A > B THEN FNMAX = A ELSE FNMAX = B
30 FNEND
40 PRINT FNMAX(3,7); FNMAX(9,2); FNMAX(-1,-4)
50 ! a body of several statements, with a loop of its own
60 DEF FNSUMSQ(N)
70   T = 0
80   FOR K = 1 TO N \ T = T + K*K \ NEXT K
90   FNSUMSQ = T
100 FNEND
110 PRINT FNSUMSQ(4); FNSUMSQ(1); FNSUMSQ(0)
120 ! the parameters are the caller's variables again afterwards
130 A = 11 \ B = 22
140 PRINT FNMAX(A,B); A; B
150 ! recursion, and a call inside a call
160 DEF FNFACT(N)
170   IF N < 2 THEN FNFACT = 1 ELSE FNFACT = N * FNFACT(N-1)
180 FNEND
190 PRINT FNFACT(5); FNFACT(0); FNMAX(FNFACT(3),FNSUMSQ(3))
200 ! a name is a name, not the keywords inside it
210 DEF FNTOTAL(X)
220   FNTOTAL = X + 1
230 FNEND
240 DEF FNORDER(X)
250   FNORDER = FNTOTAL(X) * 10
260 FNEND
270 PRINT FNTOTAL(1); FNORDER(2)
280 ! one name that starts with another, and a definition on one line
290 DEF FNQ(X) \ FNQ = X*3 \ FNEND
300 DEF FNQ2(X) \ FNQ2 = X*7 \ FNEND
310 PRINT FNQ(2); FNQ2(2)
320 ! an FNEND after a THEN is how a body leaves early, so the block
330 ! ends at the last FNEND and not at that one
340 DEF FNFIRST(N)
350   FOR K = 1 TO 100
360     FNFIRST = K
370     IF K*K >= N THEN FNEND
380   NEXT K
390 FNEND
400 PRINT FNFIRST(10); FNFIRST(1); FNSUMSQ(2)
410 ! a string function, with a string parameter, beside the one-line form
420 DEF FNPAD$(S$,W)
430   P$ = S$
440   IF LEN(P$) >= W THEN GOTO 470
450   P$ = P$ + "."
460   GOTO 440
470   FNPAD$ = P$
480 FNEND
490 DEF FNTWICE(X) = X*2
500 PRINT FNPAD$("AB",6); "|"; FNTWICE(21); FNPAD$("",3)
510 ! a value of the caller's, in the pool, held across a call whose
520 ! body fills the pool and so compacts it
530 DEF FNRUN$(N)
540   FOR M = 1 TO 5
550     T$ = ""
560     FOR J = 1 TO N \ T$ = T$ + "X" \ NEXT J
570   NEXT M
580   FNRUN$ = T$
590 FNEND
600 DIM W$(10)
610 P$ = "" \ FOR J = 1 TO 200 \ P$ = P$ + "P" \ NEXT J
620 FOR I = 0 TO 10 \ W$(I) = P$ \ NEXT I
630 P$ = ""
640 FOR L = 1 TO 3
650   Z$ = "HELD"
660   PRINT SEG$(Z$ + FNRUN$(120), 1, 6); LEN(Z$ + FNRUN$(120));
670   PRINT POS(Z$ + FNRUN$(9), "DX", 1); LEN(W$(7));
680 NEXT L
690 PRINT
700 ! the one-statement form calling the other, and a call among the
710 ! items of a PRINT to a file, whose body prints to the terminal
720 DEF FNPLUS(X) = FNTOTAL(X) + 100
730 DEF FNSAY(X)
740   PRINT "SAID";
750   FNSAY = X
760 FNEND
770 OPEN "deffn.tmp" FOR OUTPUT AS FILE #1
780 PRINT #1, FNPLUS(1); FNSAY(9)
790 CLOSE #1
800 PRINT \ OPEN "deffn.tmp" FOR INPUT AS FILE #2 \ LINPUT #2, R$ \ CLOSE #2
810 PRINT "FILE:"; R$
820 ! GOSUB out of a body and back, and READ inside one
830 DEF FNVIA(X)
840   GOSUB 1100
850   READ D
860   FNVIA = X + Q + D
870 FNEND
880 PRINT FNVIA(1); FNVIA(2)
890 ! a definition below the lines that call it, and one in a condition
900 IF FNMAX(2,5) = 5 THEN PRINT "COND" ELSE PRINT "BAD"
910 PRINT FNLATE(6)
920 GOTO 970
930 DEF FNLATE(X)
940   FNLATE = X + 0.5
950 FNEND
960 PRINT "NOT REACHED"
970 ! END inside a body ends the program
980 DEF FNOVER(X)
990   PRINT "IN THE BODY"
1000   END
1010 FNEND
1020 PRINT FNOVER(1)
1030 PRINT "NOT REACHED EITHER"
1100 Q = 100
1110 RETURN
1120 DATA 7, 8

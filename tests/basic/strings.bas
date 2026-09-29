100 ! string pool stress: many assignments force compaction
110 DIM A$(50)
120 FOR K=1 TO 40
130 FOR I=0 TO 50
140 A$(I)=A$(I)+CHR$(65+(I+K) - 26*INT((I+K)/26))
150 IF LEN(A$(I))>10 THEN A$(I)=RIGHT$(A$(I),6)
160 NEXT I
170 B$=B$+"X" \ IF LEN(B$)>30 THEN B$=""
180 NEXT K
190 PRINT A$(0);" ";A$(1);" ";A$(50);" ";LEN(B$)
200 C$="HELLO" \ D$=C$ \ C$="WORLD" \ PRINT C$;D$
210 PRINT STR$(-12.5)+"|"+STR$(100)
220 PRINT VAL("  -3.25E2"); VAL("ABC"); ASC("A"); POS("BANANA","AN",3); TRM$("AB   ")+"|"
230 PRINT LEFT$("HELLO",2);MID$("HELLO",2,3);RIGHT$("HELLO",4);SEG$("HELLO",0,99)
240 PRINT "A"<"B"; "B"<"A"; "AB"<"ABC"; "X"="X"; "X"<>"Y"
250 E$=""
260 FOR I=1 TO 255 \ E$=E$+"." \ NEXT I
270 PRINT LEN(E$)
280 E$=E$+"!"
290 PRINT "NOT REACHED"

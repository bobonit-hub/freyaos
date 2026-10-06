100 ! An expression may nest 16 deep: parentheses, function arguments,
110 ! signs and powers each take one level, and so does the body of a DEF.
120 PRINT (((((((((((((((1)))))))))))))))
130 PRINT SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(SQR(1)))))))))))))))
140 PRINT ---------------1
150 PRINT 2^1^1^1^1^1^1^1^1^1^1^1^1^1^1^1
160 DEF FNA(X) = (((((((X+1)))))))
170 PRINT FNA(FNA(1))
180 PRINT "ONE MORE"
190 PRINT ((((((((((((((((1))))))))))))))))
200 PRINT "NOT REACHED"

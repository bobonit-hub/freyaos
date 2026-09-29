100 ! every error ends the program, so most are caught here with
110 ! ON ERROR-less means: a subroutine that prints and RETURNs is
120 ! impossible; check the messages of the ones that stop instead.
130 PRINT "START"
140 X=1E30
150 PRINT X*1E10
160 PRINT "NOT REACHED"

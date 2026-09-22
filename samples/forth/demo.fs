\ demo.fs - a tour of the Freya Forth in a few definitions.
\
\   freya:/> run forth.bin demo.fs     load the file at start up
\   forth> include demo.fs             or from the prompt
\
\ Everything here is ordinary Forth except led, ms, ticks and cpuhz,
\ which are Freya service calls.

\ ---------------------------------------------------------- printing
: star      [char] * emit ;
: stars     ( n -- )  0 do star loop ;
: triangle  ( n -- )  1+ 1 do i stars cr loop ;
: box       ( n -- )  dup 0 do dup stars cr loop drop ;

\ ---------------------------------------------------------- fizzbuzz
: fizz?     ( n -- f )  3 mod 0= ;
: buzz?     ( n -- f )  5 mod 0= ;
: fizzbuzz  ( n -- )
    1+ 1 do
        i fizz? i buzz? and if    ." FizzBuzz "
        else i fizz?        if    ." Fizz "
        else i buzz?        if    ." Buzz "
        else                      i .
        then then then
    loop cr ;

\ ------------------------------------------------------------- maths
: squared   ( n -- n*n )  dup * ;
: cubed     ( n -- n^3 )  dup squared * ;
: fact      ( n -- n! )   dup 1 > if dup 1- recurse * then ;
: gcd       ( a b -- n )  begin ?dup while tuck mod repeat ;

\ ------------------------------------------------------------- board
: mhz       cpuhz 1000000 / . ." MHz" cr ;
: blink     ( n -- )  0 do 1 led 100 ms 0 led 100 ms loop ;
: wait-key  ." press a key " key drop cr ;

\ ------------------------------------------------------------ memory
\ @ and ! reach the whole address map, so this is the real SysTick
\ counter on either board, and dump reads back the dictionary itself.
: systick   ( -- n )  $E000E018 @ ;
: .free     unused . ." dictionary bytes free" cr ;

." demo.fs loaded: try  5 triangle  15 fizzbuzz  6 fact .  mhz  .free" cr

# httpd.sh - keep the web console up. httpd exits with status 3 once it
# has stored a firmware upload; update.sh installs it and the server
# starts again. Any other exit, and Ctrl-C, end the loop.
loop true
run("/spi1/httpd.bin", "/spi1/www")
if $? /= 3
break
end
source("/spi1/update.sh")
end

# update.sh - the firmware update. httpd stores an upload in
# /spi1/firmware.bin and the checksum the browser sent in
# /spi1/firmware.sum, then exits with status 3. This script checks the
# stored file against that sum, install()s it into the program flash
# region and logs one line with log(). The file and the sum are
# removed after a successful install, and when the checksum does not
# match. An install that fails leaves them, so the script can be run again.
# install() is refused from a page, so httpd.sh sources this after
# httpd has exited; source("/spi1/update.sh") runs it by hand.
set fw "/spi1/firmware.bin"
set f -1
set f open("/spi1/firmware.sum")
if $f < 0
set msg "nothing to install"
else
set want read($f)
set f close($f)
set have hex(file_checksum($fw))
if $want == $have
if install($fw)
set msg "installed, checksum " + $have
rm($fw, "/spi1/firmware.sum")
else
set msg "install failed, checksum " + $have + "; " + $fw + " kept"
end
else
set msg "checksum mismatch: file " + $have + ", sent " + $want
rm($fw, "/spi1/firmware.sum")
end
end
log("info", $msg)
echo($msg)

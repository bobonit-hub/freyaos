# login.sh - the password screen's test. httpd runs this for POST /login
# with the form's password as $query. password_check() compares it with
# the eight bytes password("xxxxxxxx") stored, without printing them.
# Only the word ok on the first line opens a session.
if password_check($query)
echo("ok")
else
echo("denied")
end
# The password must not stay in a shell variable.
unset query

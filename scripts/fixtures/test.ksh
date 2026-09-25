# knocsh script test: every feature once
set name KnocOS
echo hello from $name, $# arguments, first $1
if exists /home/Downloads
    echo downloads found
else
    echo no downloads
end
if $1 == apple
    echo first is apple
else
    echo first is not apple
end
for fruit in mango kiwi
    echo item $fruit
end
set n 0
while $n != 3
    inc n
    echo round $n
end
if not exists /nothing/here
    echo nothing is not there
end
ls /nothing
echo status after a failure: $?
echo one > /tmp/list.txt
echo two >> /tmp/list.txt
cat /tmp/list.txt
hello > /tmp/hello.txt
cat /tmp/hello.txt
copy /tmp/list.txt /tmp/copy.txt
move /tmp/copy.txt /tmp/moved.txt
cat /tmp/moved.txt
if hello
    echo a program can be a condition
end
exit 3
echo never runs: $name

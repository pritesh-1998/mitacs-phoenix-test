obj-m += phoenix.o

all: kernel userspace

kernel:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

userspace:
	gcc -Wall -o phoenix_ctl phoenix_ctl.c

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f phoenix_ctl
all:
	avr-gcc -mmcu=attiny25 -Wall -Wextra -Os -s -o kahvivaaka.elf kahvivaaka.c && avr-objcopy -j .text -j .data -O ihex kahvivaaka.elf kahvivaaka.hex && avrdude -p t25 -c avrispmkii -e -E noreset -U flash:w:kahvivaaka.hex


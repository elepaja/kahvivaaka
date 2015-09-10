#define F_CPU 1000000UL

#include <stdlib.h>
#include <stdint.h>
#include <util/delay.h>
#include <avr/sleep.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/cpufunc.h>
#include <avr/pgmspace.h>

#define PORT_WEIGHT 2  // pin 7, PB2 = ADC1
#define PORT_TEMP 4    // pin 3, PB4 = ADC2
#define PORT_BUTTON1 0 // pin 5, PB0
#define PORT_BUTTON2 3 // pin 2, PB3
#define PORT_SERIAL  1 // pin 6, PB1

#define ADMUX_WEIGHT (1<<REFS1 | 1<<MUX0) // 1.1V reference, ADC1
#define ADMUX_TEMP   (1<<REFS1 | 1<<MUX1) // 1.1V reference, ADC2
#define ADMUX_INT    (1<<REFS1 | 1<<MUX0 | 1<<MUX1 | 1<<MUX2 | 1<<MUX3) // 1.1V reference, internal temperature sensor

#define BAUDRATE 9600


//#define ADC_TIMING_DEBUG

uint32_t readadc(const uint8_t admuxv) {
	uint32_t sum = 0;
	unsigned int i;
	ADMUX = admuxv;
#ifdef ADC_TIMING_DEBUG
	DDRB |= 1<<PORT_BUTTON1;
	PORTB |= 1<<PORT_BUTTON1;
#endif
	for(i = 0; i < 632; i++) {
		ADCSRA |= (1<<ADSC); // start conversion
		do {
		} while(ADCSRA & (1<<ADSC));
		sum += ADC;
	}
#ifdef ADC_TIMING_DEBUG
	PORTB &= ~(1<<PORT_BUTTON1);
#endif
	return sum;
}


void sendbyte(uint8_t b) {
	char i;
	uint16_t tb = (b << 1) | (1<<9);
	for(i = 0; i < 10; i++) {
		if(tb&1)
			PORTB |= 1<<PORT_SERIAL;
		else
			PORTB &= ~(1<<PORT_SERIAL);
		tb >>= 1;
		_delay_us(1000000 / BAUDRATE  -7);
	}
}


void send32(uint32_t w) {
	sendbyte(0xFF & w);
	sendbyte(0xFF & w >> 8);
	sendbyte(0xFF & w >> 16);
	sendbyte(0xFF & w >> 24);
}


int main(void) {
	for(;;) {
		uint32_t v1, v2, v3;
		DDRB = 1<<PORT_SERIAL;
		PORTB = ~(1<<PORT_WEIGHT); // enable pullup in all pins except weight input

		ADCSRA = (1<<ADPS1) | (1<<ADPS0)  // prescale 1MHz/8 = 125 kHz
		//       | (1<<ADIE)                // ADC interrupt enable
		;
		ADMUX = (1<<REFS1); // use internal 1.1V reference
		ADCSRB = 0;
		ADCSRA |= 1<<ADEN;  // enable adc
		DIDR0 = (1<<PORT_WEIGHT) | (1<<PORT_TEMP); // disable digital inputs on adc pins

		v1 = readadc(ADMUX_WEIGHT);
		v2 = readadc(ADMUX_TEMP);
		v3 = readadc(ADMUX_INT);
		sendbyte(0xFF);
		sendbyte(0xFF);
		sendbyte(0xFE);
		sendbyte(PINB);
		send32(v1);
		send32(v2);
		send32(v3);
	}
}


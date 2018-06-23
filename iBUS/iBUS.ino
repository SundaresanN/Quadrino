

#include "C:/Users/Aryan/Projects/Quadrino/FlightCtrl/FlightCtrl.h"
#include "iBus.h"

void setup() {
	Serial.begin(115200);
	setup_recv();
	setup_timer();
}

void loop() {
	uint16_t tmp = TCNT1;
	process_usart_data();
	uint16_t el = TCNT1 - tmp;
	Serial.print(el);
	Serial.print(' ');
	
	for (uint8_t i = 0; i < 16; i++) { 
		Serial.print(recv[i]);
		Serial.print(' ');
	}
	Serial.println(' ');
	
	uint16_t s = TCNT1 + 10000;
	while (TCNT1 < s);
	
}
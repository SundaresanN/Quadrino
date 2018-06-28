#pragma once

register uint8_t rr2 asm ("r2");

const uint8_t USART_FRAME_SIZE = 32;

int16_t recv[USART_FRAME_SIZE / 2];

/*
	The general iBus protocol is this:
	iBus is a Serial protocol that operates at 115.2 kHz.
	The first two bytes are sync/start bytes consisting off
	0x20 then 0x40. Then there are 14 channels of recv, each
	channel being 16bits. The recv is little endian so bytes
	must be swapped. After that, there are 2 bytes that are 
	check sum bytes and is just 0xFFFF subtracted by every byte
	we transfered. (I dont know what this means but I bet that 
	the answer is in these two links). Majority of this code is
	based on this:
	https://github.com/povlhp/iBus2PPM
	and this:
	https://basejunction.wordpress.com/2015/08/23/en-flysky-i6-14-channels-part1/
	Only 10 channels of the recv really means somthing to us so
	we extract that. The code for the Serial protocol is based on
	the code here:
	http://forum.arduino.cc/index.php?topic=37874.0
*/
void setup_recv(unsigned long baud) {
	uint8_t use2x = 0;
	uint16_t ubbr =  (F_CPU + 8UL * baud) / (16UL * baud) - 1UL;
	if ( (100 * (F_CPU)) > (16 * (ubbr + 1) * (100 * ubbr + ubbr * BAUD_TOL)) ) {
		use2x = 1;
		ubbr = (F_CPU + 4UL * baud) / (8UL * baud) - 1UL;
	}
	
	UBRR0L = ubbr & 0xff;
	UBRR0H = ubbr >> 8;
	if (use2x) {
		UCSR0A |= (1 << U2X0);
	} else {
		UCSR0A &= ~(1 << U2X0);
	}
	
	UCSR0B |=  (1 << RXEN0); // enable recv
	UCSR0B &= ~(1 << UDRIE0); // disable Data Register Empty interrupt
	UCSR0B |=  (1 << RXCIE0); // enable interrupt
}


/*
	I will be implementing this as a tri-state double buffer (tri-state
	from phineas and ferb).
	There are 2 buffers, as one is getting filled the other is available
	to be used by the main program. The buffers will be protected by a 
	tri-state lock. (pretty much and atomic uint8_t). These are the states
		0. Data is available for use
		1. The data will be overwritten within 5ms
		2. Data is being used by the main program
	The basic principle is this. This interrupt set one of the buffer
	state to 1, then it will start filling up the buffer. Once the buffer
	is filled, it will change the state to 0 and set the other buffer's
	state to 1. If the main program is using this other buffer, it has about
	5ms to finish using it (See basejunction's post above why it is 5ms).
	The main program will check each buffer for state 0 and use that buffer
	as its working buffer. It will set it to state 2 and continue using it. 
	The main program will encounter only 2 states: 0 and 1. If it
	encounters 1, then it will use the other buffer (because this buffer
	is being filled). The ISR will encounter all 3 states, however it will
	only act on state 1 buffer. It will find the buffer with state 1 and
	fill it up. Once it has filled up the buffer it will change this to
	state 0 and change the other's state to 1, doest matter if its a 0 or
	a 2. The main point of state 1 is minimize the delay of the input data
	to a maximum of 7ms. We dont need to label any of these as volatile
	because only one function will be using one buffer. nobody on state 0, 
	ISR on state 1, and main function on state 2
*/
uint8_t usart_buffer1[USART_FRAME_SIZE];
volatile uint8_t buff1State = 1; // this needs to be volatile because both functions can be using it
uint8_t usart_buffer2[USART_FRAME_SIZE];
volatile uint8_t buff2State = 0;

ISR(USART_RX_vect) {
	static uint8_t idx = 0;
	uint8_t data = UDR0;
	
	// Now that we have out data we should re-enable interrupts
	// However this may create nested interrupts and we dont want this
	// so we first disable USART_RX interrupt then enable global interrupts
	// UCSR0B &= ~(1 << RXCIE0);
	// sei();
	
	if (idx == 0 and data != 0x20) return;
	
	if (buff1State == 1) {
		usart_buffer1[idx++] = data;
		if (idx == USART_FRAME_SIZE) {
			// This will be atomic cause we are in an ISR
			buff1State = 0; // set this buffer as ready
			buff2State = 1;
			idx = 0;
		}
	} else /* if (buff2State == 1) */ { // One of these is guaranteed to be 1
		usart_buffer2[idx++] = data;
		if (idx == USART_FRAME_SIZE) {
			// This will be atomic cause we are in an ISR
			buff1State = 1;
			buff2State = 0; // set this buffer as ready
			idx = 0;
		}
	}
	
	// cli();
	// UCSR0B |= (1 << RXCIE0);
}

void process_usart_data() {
	// This needs to be done atomically
	// turn off global interrupts
	uint8_t* raw;
	asm volatile ("in r2, __SREG__" :::"r2");
	cli();
	if        (buff1State == 0) {
		buff1State = 2; // We technically don't need this but... its just 2 cycles
		raw = usart_buffer1;
	} else if (buff2State == 0) {
		buff2State = 2;
		raw = usart_buffer2;
	}
	asm volatile ("out __SREG__, r2" :::"r2");
	
	recv[CSUM] = raw[30] + (raw[31] << 8);
	uint16_t chksum = 0xFFFF;
	for (uint8_t ii = 0; ii < 30; ii++) {
		chksum -= raw[ii];
	}
	
	if (chksum != recv[CSUM]) // Error in trans. Use old data
		return;
	
	recv[0] = raw[0] + (raw[1] << 8);
	
	if (recv[START   ] != 0x4020) {
		recv[ROLL    ]  = 1500;
		recv[PITCH   ]  = 1500;
		recv[THROTTLE]  = 1000; // throttle should be low
		recv[YAW     ]  = 1500; // everything else should be 
		recv[VRA     ]  = 1500; // center stick position
		recv[VRB     ]  = 1500;
		recv[SWA     ]  = 1500;
		recv[SWB     ]  = 1500;
		recv[SWC     ]  = 1500;
		recv[SWD     ]  = 1500;
		return;
	}
	
	recv[ROLL    ] = raw[ 2] + (raw[ 3] << 8);
	recv[PITCH   ] = raw[ 4] + (raw[ 5] << 8);
	recv[THROTTLE] = raw[ 6] + (raw[ 7] << 8);
	recv[YAW     ] = raw[ 8] + (raw[ 9] << 8);
	// recv[VRA     ] = raw[10] + (raw[11] << 8); // Im not using these
	// recv[VRB     ] = raw[12] + (raw[13] << 8); // channels, no need 
	// recv[SWA     ] = raw[14] + (raw[15] << 8); // calculate these
	// recv[SWB     ] = raw[16] + (raw[17] << 8);
	// recv[SWC     ] = raw[18] + (raw[19] << 8);
	// recv[SWD     ] = raw[20] + (raw[21] << 8);
	// recv[11] = raw[22] + (raw[23] << 8); // 1  dummy channels 
	// recv[12] = raw[24] + (raw[25] << 8); // 2 
	// recv[13] = raw[26] + (raw[27] << 8); // 3 
	// recv[14] = raw[28] + (raw[29] << 8); // 4 
}

void mix_channels() {
	uint16_t throttle = recv[THROTTLE] * 2;
	
	pid_roll  = recv[ROLL ] - 1500;
	pid_pitch = recv[PITCH] - 1500;
	pid_yaw   = recv[YAW  ] - 1500;
	
	escfr = throttle + pid_roll - pid_pitch - pid_yaw; // (front right CCW)
	escfl = throttle - pid_roll - pid_pitch + pid_yaw; // (font left CW)
	escbr = throttle + pid_roll + pid_pitch + pid_yaw; // (back right CW)
	escbl = throttle - pid_roll + pid_pitch - pid_yaw; // (back left CCW)
	
	if      (escfr < 2000) escfr = 2000;
	else if (escfr > 4000) escfr = 4000;
	
	if      (escfl < 2000) escfl = 2000;
	else if (escfl > 4000) escfl = 4000;
	
	if      (escbr < 2000) escbr = 2000;
	else if (escbr > 4000) escbr = 4000;
	
	if      (escbl < 2000) escbl = 2000;
	else if (escbl > 4000) escbl = 4000;
}
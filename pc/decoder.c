#include <stdio.h>
#include <stdint.h>
uint32_t read32() {
	uint32_t r;
	r  = getchar();
	r |= getchar() << 8;
	r |= getchar() << 16;
	r |= getchar() << 24;
	return r;
}

int main() {
	int g2, g1;
	for(;;) {
		uint8_t buttons;
		uint32_t v1,v2,v3;
		int g = getchar();
		if(g<0) break;
		if(g2 == 0xFF && g1 == 0xFF && g == 0xFE) {
			buttons = getchar();
			v1 = read32();
			v2 = read32();
			v3 = read32();
			printf("%3d %10d %10d %10d\n", buttons, v1, v2, v3);
		}
		g2 = g1;
		g1 = g;
	}
	return 0;
}


#pragma once
/* Returns the next required wire length; zero means an invalid packet. */
static unsigned QbtWireLength(const unsigned char *p, unsigned have) {
 unsigned header, total;
 if (!have) return 1;
 switch (p[0]) {
 case 0xfc: case 0xfd: case 0xfe: return 1;
 case 4: header=3; break;
 case 2: header=5; break;
 case 0x34: header=4; break;
 default: return 0;
 }
 if (have<header) return header;
 total=header+(p[0]==2 ? (unsigned)p[3]+((unsigned)p[4]<<8) : p[header-1]);
 return total<=4097 ? total : 0;
}

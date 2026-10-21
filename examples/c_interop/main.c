#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t packet_checksum(const uint8_t *bytes, size_t count);
extern const char *greeting(void);
extern uint32_t payload_length(const uint8_t *bytes, size_t available);

int main(void) {
    const uint8_t payload[] = {10, 11, 21};
    const uint8_t packet[] = {'A', 'S', 1, 0, 0, 3, 10, 11, 21};
    if (packet_checksum(payload, sizeof(payload)) != 42) return 1;
    if (packet_checksum(NULL, 0) != 0) return 2;
    if (strcmp(greeting(), "hello from AsterASM 1.2.1") != 0) return 3;
    if (payload_length(packet, sizeof(packet)) != 3) return 4;
    if (payload_length(packet, 5) != 0) return 5;
    if (payload_length(packet, 7) != 0) return 6;
    puts(greeting());
    return 0;
}

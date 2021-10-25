#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "naomi/video.h"
#include "naomi/maple.h"
#include "naomi/eeprom.h"
#include "naomi/system.h"
#include "naomi/timer.h"
#include "naomi/dimmcomms.h"

#define MAX_OUTSTANDING_PACKETS 268
#define MAX_PACKET_LENGTH 253

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

typedef struct
{
    uint8_t data[MAX_PACKET_LENGTH];
    unsigned int len;
} packet_t;

typedef struct
{
    packet_t *pending_packets[MAX_OUTSTANDING_PACKETS];
    packet_t *received_packets[MAX_OUTSTANDING_PACKETS];
    uint8_t pending_send_data[MAX_PACKET_LENGTH];
    int pending_send_size;
    int pending_send_location;
    uint8_t pending_recv_data[MAX_PACKET_LENGTH];
    int pending_recv_size;
    int pending_recv_location;
    unsigned int success_sent;
    unsigned int success_received;
    unsigned int cancelled_packets;
    unsigned int checksum_errors;
} packetlib_state_t;

packetlib_state_t packetlib_state;

uint32_t peek_memory(unsigned int address, int size);
void poke_memory(unsigned int address, int size, uint32_t data);

void packetlib_init()
{
    // Initialize packet library.
    for (int i = 0; i < MAX_OUTSTANDING_PACKETS; i++) {
        packetlib_state.pending_packets[i] = 0;
        packetlib_state.received_packets[i] = 0;
    }

    packetlib_state.pending_send_size = 0;
    packetlib_state.pending_recv_size = 0;
    packetlib_state.success_sent = 0;
    packetlib_state.success_received = 0;
    packetlib_state.cancelled_packets = 0;
    packetlib_state.checksum_errors = 0;

    // Attach our handlers for sending/receiving data.
    dimm_comms_attach_hooks(&peek_memory, &poke_memory);
}

void packetlib_free()
{
    // No more receiving messages.
    dimm_comms_detach_hooks();

    // Free any outstanding packets.
    for (int i = 0; i < MAX_OUTSTANDING_PACKETS; i++) {
        if (packetlib_state.pending_packets[i] != 0) {
            free(packetlib_state.pending_packets[i]);
            packetlib_state.pending_packets[i] = 0;
        }
        if (packetlib_state.received_packets[i] == 0) {
            free(packetlib_state.received_packets[i]);
            packetlib_state.received_packets[i] = 0;
        }
    }
}

typedef struct
{
    unsigned int packets_sent;
    unsigned int packets_received;
    unsigned int packets_cancelled;
    unsigned int checksum_errors;
    unsigned int packets_pending_send;
    unsigned int packets_pending_receive;
    unsigned int send_in_progress;
    unsigned int receive_in_progress;
} packetlib_stats_t;

packetlib_stats_t packetlib_stats()
{
    packetlib_stats_t stats;

    stats.packets_sent = packetlib_state.success_sent;
    stats.packets_received = packetlib_state.success_received;
    stats.packets_cancelled = packetlib_state.cancelled_packets;
    stats.checksum_errors = packetlib_state.checksum_errors;
    stats.packets_pending_send = 0;
    stats.packets_pending_receive = 0;
    for (int i = 0; i < MAX_OUTSTANDING_PACKETS; i++) {
        if (packetlib_state.pending_packets[i] != 0) {
            stats.packets_pending_send++;
        }
        if (packetlib_state.received_packets[i] != 0) {
            stats.packets_pending_receive++;
        }
    }
    stats.send_in_progress = packetlib_state.pending_send_size > 0 ? 1 : 0;
    stats.receive_in_progress = packetlib_state.pending_recv_size > 0 ? 1 : 0;
    return stats;
}

int packetlib_send(void *data, unsigned int length)
{
    if (length == 0 || length > MAX_PACKET_LENGTH)
    {
        return -1;
    }

    for (int i = 0; i < MAX_OUTSTANDING_PACKETS; i ++)
    {
        if (packetlib_state.pending_packets[i] == 0)
        {
            packetlib_state.pending_packets[i] = (packet_t *)malloc(sizeof(packet_t));
            memcpy(packetlib_state.pending_packets[i]->data, data, length);
            packetlib_state.pending_packets[i]->len = length;
            return 0;
        }
    }

    return -2;
}

int packetlib_recv(void *data, unsigned int *length)
{
    for (int i = 0; i < MAX_OUTSTANDING_PACKETS; i ++)
    {
        if (packetlib_state.received_packets[i] != 0)
        {
            // Copy the data over.
            memcpy(data, packetlib_state.received_packets[i]->data, packetlib_state.received_packets[i]->len);
            *length = packetlib_state.received_packets[i]->len;

            // Free up the packet for later use.
            free(packetlib_state.received_packets[i]);
            packetlib_state.received_packets[i] = 0;

            // Success!
            return 0;
        }
    }

    return -1;
}

void *packetlib_peek(int packetno, unsigned int *length)
{
    if (packetlib_state.received_packets[packetno] != 0)
    {
        *length = packetlib_state.received_packets[packetno]->len;
        return packetlib_state.received_packets[packetno]->data;
    }
    else
    {
        *length = 0;
        return 0;
    }
}

void packetlib_discard(int packetno)
{
    if (packetlib_state.received_packets[packetno] != 0)
    {
        // Free up the packet for later use.
        free(packetlib_state.received_packets[packetno]);
        packetlib_state.received_packets[packetno] = 0;
    }
}

uint32_t checksum_add(uint32_t value)
{
    uint8_t sum = ((value & 0xFF) + ((value >> 8) & 0xFF)) & 0xFF;
    return (((~sum) & 0xFF) << 16) | (value & 0x0000FFFF);
}

int checksum_verify(uint32_t value)
{
    uint8_t sum = ((value & 0xFF) + ((value >> 8) & 0xFF)) & 0xFF;
    return (((~sum) & 0xFF) == ((value >> 16) & 0xFF)) ? 1 : 0;
}

uint32_t read_data()
{
    // If we have no data to send, bail out.
    if (packetlib_state.pending_send_size == 0)
    {
        return 0;
    }
    if (packetlib_state.pending_send_size == packetlib_state.pending_send_location)
    {
        return 0;
    }

    // First, construct the location portion of the packet.
    uint32_t response = ((packetlib_state.pending_send_location + 1) << 24) & 0xFF000000;

    // Now, until we run out of data, stick more into the buffer.
    for (int i = 16; i >= 0; i -= 8)
    {
        if (packetlib_state.pending_send_size == packetlib_state.pending_send_location)
        {
            return response;
        }

        response |= (packetlib_state.pending_send_data[packetlib_state.pending_send_location++] & 0xFF) << i;
    }

    return response;
}

uint32_t read_send_status()
{
    // Read the send status register for our communications protocol.
    // Top 8 bits is all 0s to make sure that it doesn't get confused
    // with a data send/receive which has a top byte that can only be
    // a nonzero value from 1-254. The next 8 bits are a simple inverted
    // checksum of the rest of the packet. Next 8 bits is the size of
    // a pending packet to send from naomi to host. Next 8 bits is the
    // location of the send in progress.
    uint32_t regdata = 0;

    if (packetlib_state.pending_send_size == 0)
    {
        // Attempt to find another packet to send.
        for (int i = 0; i < MAX_OUTSTANDING_PACKETS; i ++)
        {
            if (packetlib_state.pending_packets[i] != 0)
            {
                // Grab the pending packet information, set up the transfer.
                memcpy(packetlib_state.pending_send_data, packetlib_state.pending_packets[i]->data, packetlib_state.pending_packets[i]->len);
                packetlib_state.pending_send_size = packetlib_state.pending_packets[i]->len;
                packetlib_state.pending_send_location = 0;

                // Get rid of the packet on the pending packets buffer.
                free(packetlib_state.pending_packets[i]);
                packetlib_state.pending_packets[i] = 0;
                break;
            }
        }
    }

    if (packetlib_state.pending_send_size != 0)
    {
        regdata = ((packetlib_state.pending_send_size << 8) & 0xFF00) | (packetlib_state.pending_send_location & 0x00FF);
    }

    // Actually return the data.
    return checksum_add(regdata);
}

void write_send_status(uint32_t status)
{
    // Write the send status register for our communications protocol. The only
    // things that the host is allowed to modify is the current location, so it
    // can rewind for missed data. It can also acknowledge the transfer by setting
    // the current location to the length of the packet.
    if (checksum_verify(status))
    {
        unsigned int location = status & 0xFF;
        if (location < packetlib_state.pending_send_size)
        {
            // Host is requesting a resend of some data.
            packetlib_state.pending_send_location = location;
        }
        else if(location == packetlib_state.pending_send_size)
        {
            // Transfer succeeded! Get rid of the current pending transfer.
            packetlib_state.pending_send_size = 0;
            packetlib_state.pending_send_location = 0;
            packetlib_state.success_sent ++;
        }
    }
    else
    {
        packetlib_state.checksum_errors ++;
    }
}

void write_data(uint32_t data)
{
    // Much like sending data to the host, the top byte is the location + 1 (so it
    // can never be 0x00 or 0xFF, two values commonly seen when the net dimm firmware
    // fails to read a packet or reads a copy of another register), then the next
    // three bytes are optionally the packet data. Unlike the host which can reassemble
    // packets in any order, we aren't so powerful. We simply check to see if the location
    // is where we left off. If it is, we accept thet packet. If not, then we ignore it.
    // The host is responsible for checking the receive status register after transferring
    // to see if it needs to rewind, or if the transfer succeeded.
    if (packetlib_state.pending_recv_size != 0)
    {
        unsigned int location = ((data >> 24) & 0xFF);
        if (location == 0xFF || location == 0x00)
        {
            // This is a bogus packet.
            return;
        }

        // Get the actual location.
        location -= 1;
        if (location != packetlib_state.pending_recv_location)
        {
            // We missed some data.
            return;
        }

        // Copy data until we have no more data left to copy, or until we hit the end of
        // the packet. If we hit the end of the packet, acknowledge the successful receipt
        // by setting the current location and size to zero.
        for (int i = 16; i >= 0; i -= 8)
        {
            packetlib_state.pending_recv_data[packetlib_state.pending_recv_location++] = (data >> i) & 0xFF;

            if (packetlib_state.pending_recv_size == packetlib_state.pending_recv_location)
            {
                // We did it! Add to the pending receive queue.
                for (int j = 0; j < MAX_OUTSTANDING_PACKETS; j ++)
                {
                    if (packetlib_state.received_packets[j] == 0)
                    {
                        // Copy the packet information so userspace can read it.
                        packetlib_state.received_packets[j] = (packet_t *)malloc(sizeof(packet_t));
                        memcpy(packetlib_state.received_packets[j]->data, packetlib_state.pending_recv_data, packetlib_state.pending_recv_size);
                        packetlib_state.received_packets[j]->len = packetlib_state.pending_recv_size;
                        break;
                    }
                }

                // Mark that the packet was received.
                packetlib_state.pending_recv_size = 0;
                packetlib_state.pending_recv_location = 0;
                packetlib_state.success_received ++;

                return;
            }
        }
    }
}

uint32_t read_recv_status()
{
    // Read the receive status register. This is a carbon copy of the read_send_status
    // register except for the values are for receiving a packet from the host instead
    // of sending a packet to the host.
    uint32_t regdata = 0;

    if (packetlib_state.pending_recv_size != 0)
    {
        regdata = ((packetlib_state.pending_recv_size << 8) & 0xFF00) | (packetlib_state.pending_recv_location & 0x00FF);
    }

    // Actually return the data.
    return checksum_add(regdata);
}

void write_recv_status(uint32_t status)
{
    // Write the receive status register. This is similar to write_send_status in that
    // the host is allowed to send the length to initiate a transfer. However, it should
    // only do so if the length is currently 0, and it can only change the length from
    // 0 to some packet length to be received. It is responsible for checking the current
    // location to see if data needs to be rewound, and if it is sending a packet and the
    // length goes back to 0 it means that the packet has been received successfully. The
    // host does not have access to change the location. If the host determines that a
    // previous transfer was mid-way through and it does not have knowledge of it, then
    // it should cancel the transfer by writing all 0's to this register.
    if (checksum_verify(status))
    {
        unsigned int size = (status >> 8) & 0xFF;
        if (size > 0 && size <= MAX_PACKET_LENGTH)
        {
            if (packetlib_state.pending_recv_size == 0)
            {
                // Start a new transfer, but only if we have room in our receive queue.
                for (int i = 0; i < MAX_OUTSTANDING_PACKETS; i++)
                {
                    if (packetlib_state.received_packets[i] == 0)
                    {
                        packetlib_state.pending_recv_size = size;
                        packetlib_state.pending_recv_location = 0;
                        return;
                    }
                }
            }
        }
        else if (size == 0)
        {
            // Cancel any pending transfer.
            if (packetlib_state.pending_recv_size != 0)
            {
                packetlib_state.pending_recv_size = 0;
                packetlib_state.pending_recv_location = 0;
                packetlib_state.cancelled_packets ++;
            }
        }
    }
    else
    {
        packetlib_state.checksum_errors ++;
    }
}

uint32_t peek_memory(unsigned int address, int size)
{
    if (size == 4)
    {
        if ((address & 0xFFFFFF) == 0xC0DE10) {
            // Read data register.
            return read_data();
        }
        else if ((address & 0xFFFFFF) == 0xC0DE20) {
            // Read status register.
            return read_send_status();
        }
        else if ((address & 0xFFFFFF) == 0xC0DE30) {
            // Read status register.
            return read_recv_status();
        }
    }

    // The net dimm seems a lot happier if we return nonzero values for
    // its random reads that it does.
    return 0xFFFFFFFF;
}

void poke_memory(unsigned int address, int size, uint32_t data)
{
    if (size == 4)
    {
        if ((address & 0xFFFFFF) == 0xC0DE10) {
            // Write data register.
            write_data(data);
        }
        else if ((address & 0xFFFFFF) == 0xC0DE20) {
            // Write status register.
            write_send_status(data);
        }
        else if ((address & 0xFFFFFF) == 0xC0DE30) {
            // Write status register.
            write_recv_status(data);
        }
    }
}

void packetlib_render_stats(char *buffer)
{
    // Display information about current packet library.
    packetlib_stats_t stats = packetlib_stats();
    sprintf(
        buffer,
        "Total packets sent: %d\nTotal packets received: %d\n"
        "Cancelled packets: %d\nChecksum errors: %d\n"
        "Pending packets: %d to send, %d to receive\n"
        "Send in progress: %s\nReceive in progress: %s",
        stats.packets_sent,
        stats.packets_received,
        stats.packets_cancelled,
        stats.checksum_errors,
        stats.packets_pending_send,
        stats.packets_pending_receive,
        stats.send_in_progress ? "yes" : "no",
        stats.receive_in_progress ? "yes" : "no"
    );
}

#define MAX_MESSAGE_LENGTH 0xFFFF
#define MESSAGE_HEADER_LENGTH 8
#define MAX_MESSAGE_DATA_LENGTH (MAX_PACKET_LENGTH - MESSAGE_HEADER_LENGTH)
#define MESSAGE_ID_LOC 0
#define MESSAGE_SEQ_LOC 2
#define MESSAGE_LEN_LOC 4
#define MESSAGE_LOC_LOC 6
#define MESSAGE_DATA_LOC 8

int message_send(uint16_t type, void * data, unsigned int length)
{
    uint8_t buffer[MAX_PACKET_LENGTH];
    static uint16_t sequence = 1;

    if (length > MAX_MESSAGE_LENGTH)
    {
        return -3;
    }

    // We always want to run this loop at least one time, so we can send
    // packets of 0 bytes in length.
    for (unsigned int loc = 0; (loc == 0 || loc < length); loc += MAX_MESSAGE_DATA_LENGTH)
    {
        unsigned int packet_len = length - loc;
        if (packet_len > MAX_MESSAGE_DATA_LENGTH)
        {
            packet_len = MAX_MESSAGE_DATA_LENGTH;
        }

        // Set up packet type in header.
        uint16_t tmp = type;
        memcpy(&buffer[MESSAGE_ID_LOC], &tmp, 2);

        // Set up sequence number in header.
        memcpy(&buffer[MESSAGE_SEQ_LOC], &sequence, 2);

        // Set up packet length in header.
        tmp = length;
        memcpy(&buffer[MESSAGE_LEN_LOC], &tmp, 2);

        // Set up current packet location in header.
        tmp = loc;
        memcpy(&buffer[MESSAGE_LOC_LOC], &tmp, 2);

        if (packet_len > 0)
        {
            // Finally, copy the data in.
            memcpy(&buffer[MESSAGE_DATA_LOC], ((uint8_t *)data) + loc, packet_len);
        }

        // Now, send the packet.
        if (packetlib_send(buffer, packet_len + MESSAGE_HEADER_LENGTH) != 0)
        {
            return -4;
        }
    }

    // We finished this packet, set the sequence number to something else for
    // the next packet.
    sequence ++;
    if (sequence == 0)
    {
        // Don't want sequence ID 0 for reassembly purposes.
        sequence = 1;
    }

    return 0;
}

int message_recv(uint16_t *type, void ** data, unsigned int *length)
{
    // Figure out if there is a packet worth assembling. This is a really gross,
    // inefficient algorithm, but whatever its good enough for now.
    uint8_t *reassembled_data = 0;
    int success = -5;
    uint16_t seen_packet_sequences[MAX_OUTSTANDING_PACKETS];
    uint8_t *seen_positions[MAX_OUTSTANDING_PACKETS];
    uint16_t seen_packet_lengths[MAX_OUTSTANDING_PACKETS];
    memset(seen_packet_sequences, 0, sizeof(uint16_t) * MAX_OUTSTANDING_PACKETS);
    memset(seen_positions, 0, sizeof(uint8_t *) * MAX_OUTSTANDING_PACKETS);
    memset(seen_packet_lengths, 0, sizeof(uint16_t) * MAX_OUTSTANDING_PACKETS);

    for (unsigned int pkt = 0; pkt < MAX_OUTSTANDING_PACKETS; pkt++)
    {
        // Grab the potential packet we could receive.
        unsigned int pkt_length = 0;
        uint8_t *pkt_data = packetlib_peek(pkt, &pkt_length);
        if (pkt_data == 0)
        {
            // No data for this packet.
            continue;
        }
        if (pkt_length < MESSAGE_HEADER_LENGTH)
        {
            // Toss bogus packet.
            packetlib_discard(pkt);
            continue;
        }

        // Grab the sequence number from this packet.
        uint16_t sequence;
        int index = -1;
        memcpy(&sequence, &pkt_data[MESSAGE_SEQ_LOC], 2);

        if (sequence == 0)
        {
            // Toss bogus packet.
            packetlib_discard(pkt);
            continue;
        }

        // Grab the length and needed total packets for this packet.
        uint16_t msg_length;
        memcpy(&msg_length, &pkt_data[MESSAGE_LEN_LOC], 2);
        unsigned int num_packets_needed = (msg_length + (MAX_MESSAGE_DATA_LENGTH - 1)) / MAX_MESSAGE_DATA_LENGTH;

        // Find the positions data for this sequence.
        for (unsigned int i = 0; i < MAX_OUTSTANDING_PACKETS; i++)
        {
            if (seen_packet_sequences[i] == sequence)
            {
                index = i;
                break;
            }
            if (seen_packet_sequences[i] == 0)
            {
                // The index doesn't exist, lets create it.
                index = i;

                // Calculate how many parts of the message we need to see.
                seen_packet_sequences[index] = sequence;
                seen_packet_lengths[index] = msg_length;
                if (num_packets_needed > 0)
                {
                    seen_positions[index] = malloc(num_packets_needed);
                    memset(seen_positions[index], 0, num_packets_needed);
                }
                break;
            }
        }

        if (num_packets_needed > 0 && index >= 0)
        {
            // Now, mark the particular portion of this packet as present.
            uint16_t location;
            memcpy(&location, &pkt_data[MESSAGE_LOC_LOC], 2);
            seen_positions[index][location / MAX_MESSAGE_DATA_LENGTH] = 1;
        }
    }

    // Now that we've gathered up which packets we have, see if any packets
    // we care about are fully received.
    for (unsigned int index = 0; index < MAX_OUTSTANDING_PACKETS; index++)
    {
        if (seen_packet_sequences[index] == 0)
        {
            // We ran out of packet sequences we're tracking.
            break;
        }

        unsigned int num_packets_needed = (seen_packet_lengths[index] + (MAX_MESSAGE_DATA_LENGTH - 1)) / MAX_MESSAGE_DATA_LENGTH;
        int ready = 1;

        for (unsigned int i = 0; i < num_packets_needed; i++)
        {
            if (!seen_positions[index][i])
            {
                // This packet is not ready.
                ready = 0;
                break;
            }
        }

        if (ready)
        {
            // This packet is ready!
            if (seen_packet_lengths[index] > 0)
            {
                reassembled_data = malloc(seen_packet_lengths[index]);
            }
            *data = reassembled_data;
            *length = seen_packet_lengths[index];

            for (unsigned int pkt = 0; pkt < MAX_OUTSTANDING_PACKETS; pkt++)
            {
                // Grab the potential packet we could receive.
                unsigned int pkt_length = 0;
                uint8_t *pkt_data = packetlib_peek(pkt, &pkt_length);
                if (pkt_data == 0 || pkt_length < MESSAGE_HEADER_LENGTH)
                {
                    // No data for this packet.
                    continue;
                }

                // Grab the sequence number from this packet.
                uint16_t sequence;
                memcpy(&sequence, &pkt_data[MESSAGE_SEQ_LOC], 2);

                if (sequence != seen_packet_sequences[index])
                {
                    // This packet is not one of the ones we're after.
                    continue;
                }

                // Grab the type from this packet. This is inefficient since we
                // only need to do it once, but whatever. Its two whole bytes and
                // this entire reassembly algorithm could use work.
                memcpy(type, &pkt_data[MESSAGE_ID_LOC], 2);

                if (seen_packet_lengths[index] > 0)
                {
                    // Grab the location from this packet, so we can copy it into
                    // the right spot in the destination.
                    uint16_t location;
                    memcpy(&location, &pkt_data[MESSAGE_LOC_LOC], 2);

                    // Actually copy it.
                    memcpy(reassembled_data + location, &pkt_data[MESSAGE_DATA_LOC], pkt_length - MESSAGE_HEADER_LENGTH);
                }

                // We don't need this packet anymore, since we received it.
                packetlib_discard(pkt);
            }

            // We finished assembling the packet, lets return it!
            success = 0;
            break;
        }
    }

    // Need to free a bunch of stuff.
    for (unsigned int index = 0; index < MAX_OUTSTANDING_PACKETS; index++)
    {
        if (seen_positions[index])
        {
            free(seen_positions[index]);
        }
    }

    // Return the possibly reassembled packet.
    return success;
}

#define CONFIG_MEMORY_LOCATION 0x0D000000
#define GAMES_POINTER_LOC 0
#define GAMES_COUNT_LOC 4
#define ENABLE_ANALOG_LOC 8
#define ENABLE_DEBUG_LOC 12
#define DEFAULT_SELECTION_LOC 16
#define SYSTEM_REGION_LOC 20
#define USE_FILENAMES_LOC 24

typedef struct __attribute__((__packed__))
{
    char name[128];
    uint8_t serial[4];
    unsigned int id;
} games_list_t;

typedef struct __attribute__((__packed__))
{
    uint32_t game_list_offset;
    uint32_t games_count;
    uint32_t enable_analog;
    uint32_t enable_debug;
    uint32_t boot_selection;
    uint32_t system_region;
    uint32_t use_filenames;
    uint8_t joy1_hcenter;
    uint8_t joy1_vcenter;
    uint8_t joy2_hcenter;
    uint8_t joy2_vcenter;
    uint8_t joy1_hmin;
    uint8_t joy1_hmax;
    uint8_t joy1_vmin;
    uint8_t joy1_vmax;
    uint8_t joy2_hmin;
    uint8_t joy2_hmax;
    uint8_t joy2_vmin;
    uint8_t joy2_vmax;
    uint32_t fallback_font_offset;
    uint32_t fallback_font_size;
} config_t;

config_t *get_config()
{
    return (config_t *)CONFIG_MEMORY_LOCATION;
}

games_list_t *get_games_list(unsigned int *count)
{
    // Index into config memory to grab the count of games, as well as the offset pointer
    // to where the games blob is.
    config_t *config = get_config();
    *count = config->games_count;
    return (games_list_t *)(CONFIG_MEMORY_LOCATION + config->game_list_offset);
}

uint8_t *get_fallback_font(unsigned int *size)
{
    config_t *config = get_config();
    *size = config->fallback_font_size;
    if (config->fallback_font_size && config->fallback_font_offset)
    {
        return (uint8_t *)(CONFIG_MEMORY_LOCATION + config->fallback_font_offset);
    }
    else
    {
        return 0;
    }
}

unsigned int repeat(unsigned int cur_state, int *repeat_count, double fps)
{
    // Based on 60fps. A held button will "repeat" itself ~16x a second
    // after a 0.5 second hold delay.
    if (*repeat_count < 0)
    {
        // If we have never pushed this button, don't try repeating
        // if it happened to be held.
        return 0;
    }

    if (cur_state == 0)
    {
        // Button isn't held, no repeats.
        *repeat_count = 0;
        return 0;
    }

    // Calculate repeat values based on current FPS.
    int count = *repeat_count;
    *repeat_count = count + 1;

    int thresh = (int)(30.0 * (fps / 60.0));
    int repeat = thresh / 5;

    if (count >= thresh)
    {
        // Repeat every 1/16 second after 0.5 second.
        return (count % repeat) ? 0 : 1;
    }

    return 0;
}

void repeat_init(unsigned int pushed_state, int *repeat_count)
{
    if (pushed_state == 0)
    {
        // Haven't pushed the button yet.
        return;
    }
    if (*repeat_count < 0)
    {
        // Mark that we've seen this button pressed.
        *repeat_count = 0;
    }
}

#define MESSAGE_SELECTION 0x1000
#define MESSAGE_LOAD_SETTINGS 0x1001
#define MESSAGE_LOAD_SETTINGS_ACK 0x1002
#define MESSAGE_SAVE_CONFIG 0x1003
#define MESSAGE_SAVE_CONFIG_ACK 0x1004

#define ANALOG_CENTER 0x80
#define ANALOG_THRESH_ON 0x30
#define ANALOG_THRESH_OFF 0x20

extern uint8_t *dejavusans_ttf_data;
extern unsigned int dejavusans_ttf_len;

extern unsigned int up_png_width;
extern unsigned int up_png_height;
extern void *up_png_data;

extern unsigned int dn_png_width;
extern unsigned int dn_png_height;
extern void *dn_png_data;

extern unsigned int cursor_png_width;
extern unsigned int cursor_png_height;
extern void *cursor_png_data;

unsigned int selected_game;

#define SCREEN_MAIN_MENU 0
#define SCREEN_COMM_ERROR 1
#define SCREEN_GAME_SETTINGS_LOAD 2
#define SCREEN_GAME_SETTINGS 3
#define SCREEN_CONFIGURATION 4
#define SCREEN_CONFIGURATION_SAVE 5

#define MAX_WAIT_FOR_COMMS 3.0
#define MAX_WAIT_FOR_SAVE 5.0

typedef struct
{
    eeprom_t *settings;
    config_t *config;
    double fps;
    double animation_counter;
    double test_error_counter;
    font_t *font_18pt;
    font_t *font_12pt;
} state_t;

typedef struct
{
    // The following controls only ever need a pressed event.
    uint8_t up_pressed;
    uint8_t down_pressed;
    uint8_t left_pressed;
    uint8_t right_pressed;
    uint8_t test_pressed;
    uint8_t service_pressed;

    // The following controls need pressed and released events to detect holds.
    uint8_t start_pressed;
    uint8_t start_released;
} controls_t;

#define ANALOG_DEAD_ZONE 8

controls_t get_controls(state_t *state, int reinit)
{
    static unsigned int oldaup[2] = { 0 };
    static unsigned int oldadown[2] = { 0 };
    static unsigned int aup[2] = { 0 };
    static unsigned int adown[2] = { 0 };
    static unsigned int oldaleft[2] = { 0 };
    static unsigned int oldaright[2] = { 0 };
    static unsigned int aleft[2] = { 0 };
    static unsigned int aright[2] = { 0 };
    static int repeats[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

    if (reinit)
    {
        memset(oldaup, 0, sizeof(unsigned int) * 2);
        memset(aup, 0, sizeof(unsigned int) * 2);
        memset(oldadown, 0, sizeof(unsigned int) * 2);
        memset(adown, 0, sizeof(unsigned int) * 2);
        memset(oldaleft, 0, sizeof(unsigned int) * 2);
        memset(aleft, 0, sizeof(unsigned int) * 2);
        memset(oldaright, 0, sizeof(unsigned int) * 2);
        memset(aright, 0, sizeof(unsigned int) * 2);
        
        for (unsigned int i = 0; i < sizeof(repeats) / sizeof(repeats[0]); i++)
        {
            repeats[i] = -1;
        }
    }

    // First, poll the buttons and act accordingly.
    maple_poll_buttons();
    jvs_buttons_t pressed = maple_buttons_pressed();
    jvs_buttons_t held = maple_buttons_current();
    jvs_buttons_t released = maple_buttons_released();

    // Calculate calibrations.
    unsigned int joy1_hminthresh = (state->config->joy1_hmin + state->config->joy1_hcenter) / 2;
    unsigned int joy1_hmaxthresh = (state->config->joy1_hmax + state->config->joy1_hcenter) / 2;
    unsigned int joy1_vminthresh = (state->config->joy1_vmin + state->config->joy1_vcenter) / 2;
    unsigned int joy1_vmaxthresh = (state->config->joy1_vmax + state->config->joy1_vcenter) / 2;
    unsigned int joy2_hminthresh = (state->config->joy2_hmin + state->config->joy2_hcenter) / 2;
    unsigned int joy2_hmaxthresh = (state->config->joy2_hmax + state->config->joy2_hcenter) / 2;
    unsigned int joy2_vminthresh = (state->config->joy2_vmin + state->config->joy2_vcenter) / 2;
    unsigned int joy2_vmaxthresh = (state->config->joy2_vmax + state->config->joy2_vcenter) / 2;

    // Also calculate analog thresholds so we can emulate joystick with analog.
    if (state->config->enable_analog)
    {
        if (held.player1.analog1 < joy1_vminthresh)
        {
            aup[0] = 1;
        }
        else if (held.player1.analog1 > (joy1_vminthresh + ANALOG_DEAD_ZONE))
        {
            aup[0] = 0;
        }

        if (held.player2.analog1 < joy2_vminthresh)
        {
            aup[1] = 1;
        }
        else if (held.player2.analog1 > (joy2_vminthresh + ANALOG_DEAD_ZONE))
        {
            aup[1] = 0;
        }

        if (held.player1.analog1 > joy1_vmaxthresh)
        {
            adown[0] = 1;
        }
        else if (held.player1.analog1 < (joy1_vmaxthresh - ANALOG_DEAD_ZONE))
        {
            adown[0] = 0;
        }

        if (held.player2.analog1 > joy2_vmaxthresh)
        {
            adown[1] = 1;
        }
        else if (held.player2.analog1 < (joy2_vmaxthresh - ANALOG_DEAD_ZONE))
        {
            adown[1] = 0;
        }

        if (held.player1.analog2 < joy1_hminthresh)
        {
            aleft[0] = 1;
        }
        else if (held.player1.analog2 > (joy1_hminthresh + ANALOG_DEAD_ZONE))
        {
            aleft[0] = 0;
        }

        if (held.player2.analog2 < joy2_hminthresh)
        {
            aleft[1] = 1;
        }
        else if (held.player2.analog2 > (joy2_hminthresh + ANALOG_DEAD_ZONE))
        {
            aleft[1] = 0;
        }

        if (held.player1.analog2 > joy1_hmaxthresh)
        {
            aright[0] = 1;
        }
        else if (held.player1.analog2 < (joy1_hmaxthresh - ANALOG_DEAD_ZONE))
        {
            aright[0] = 0;
        }

        if (held.player2.analog2 > joy2_hmaxthresh)
        {
            aright[1] = 1;
        }
        else if (held.player2.analog2 < (joy2_hmaxthresh - ANALOG_DEAD_ZONE))
        {
            aright[1] = 0;
        }

        // Map analogs back onto digitals.
        if (aup[0])
        {
            held.player1.up = 1;
        }
        if (aup[1])
        {
            held.player2.up = 1;
        }
        if (adown[0])
        {
            held.player1.down = 1;
        }
        if (adown[1])
        {
            held.player2.down = 1;
        }
        if (aup[0] && !oldaup[0])
        {
            pressed.player1.up = 1;
        }
        if (aup[1] && !oldaup[1])
        {
            pressed.player2.up = 1;
        }
        if (adown[0] && !oldadown[0])
        {
            pressed.player1.down = 1;
        }
        if (adown[1] && !oldadown[1])
        {
            pressed.player2.down = 1;
        }

        if (aleft[0])
        {
            held.player1.left = 1;
        }
        if (aleft[1])
        {
            held.player2.left = 1;
        }
        if (aright[0])
        {
            held.player1.right = 1;
        }
        if (aright[1])
        {
            held.player2.right = 1;
        }
        if (aleft[0] && !oldaleft[0])
        {
            pressed.player1.left = 1;
        }
        if (aleft[1] && !oldaleft[1])
        {
            pressed.player2.left = 1;
        }
        if (aright[0] && !oldaright[0])
        {
            pressed.player1.right = 1;
        }
        if (aright[1] && !oldaright[1])
        {
            pressed.player2.right = 1;
        }

        memcpy(oldaup, aup, sizeof(aup));
        memcpy(oldadown, adown, sizeof(adown));
        memcpy(oldaleft, aleft, sizeof(aleft));
        memcpy(oldaright, aright, sizeof(aright));
    }

    // Process buttons and repeats.
    controls_t controls;
    controls.up_pressed = 0;
    controls.down_pressed = 0;
    controls.left_pressed = 0;
    controls.right_pressed = 0;
    controls.start_pressed = 0;
    controls.start_released = 0;
    controls.test_pressed = 0;
    controls.service_pressed = 0;

    if (pressed.test || pressed.psw1)
    {
        controls.test_pressed = 1;
    }
    else if (pressed.player1.service || pressed.psw2 || (state->settings->system.players >= 2 && pressed.player2.service))
    {
        controls.service_pressed = 1;
    }
    else
    {
        if (pressed.player1.start || (state->settings->system.players >= 2 && pressed.player2.start))
        {
            controls.start_pressed = 1;
        }
        else if (released.player1.start || (state->settings->system.players >= 2 && released.player2.start))
        {
            controls.start_released = 1;
        }
        else
        {
            if (pressed.player1.up || (state->settings->system.players >= 2 && pressed.player2.up))
            {
                controls.up_pressed = 1;

                repeat_init(pressed.player1.up, &repeats[0]);
                repeat_init(pressed.player2.up, &repeats[1]);
            }
            else if (pressed.player1.down || (state->settings->system.players >= 2 && pressed.player2.down))
            {
                controls.down_pressed = 1;

                repeat_init(pressed.player1.down, &repeats[2]);
                repeat_init(pressed.player2.down, &repeats[3]);
            }
            if (repeat(held.player1.up, &repeats[0], state->fps) || (state->settings->system.players >= 2 && repeat(held.player2.up, &repeats[1], state->fps)))
            {
                controls.up_pressed = 1;
            }
            else if (repeat(held.player1.down, &repeats[2], state->fps) || (state->settings->system.players >= 2 && repeat(held.player2.down, &repeats[3], state->fps)))
            {
                controls.down_pressed = 1;
            }
            if (pressed.player1.left || (state->settings->system.players >= 2 && pressed.player2.left))
            {
                controls.left_pressed = 1;

                repeat_init(pressed.player1.left, &repeats[4]);
                repeat_init(pressed.player2.left, &repeats[5]);
            }
            else if (pressed.player1.right || (state->settings->system.players >= 2 && pressed.player2.right))
            {
                controls.right_pressed = 1;

                repeat_init(pressed.player1.right, &repeats[6]);
                repeat_init(pressed.player2.right, &repeats[7]);
            }
            if (repeat(held.player1.left, &repeats[4], state->fps) || (state->settings->system.players >= 2 && repeat(held.player2.left, &repeats[5], state->fps)))
            {
                controls.left_pressed = 1;
            }
            else if (repeat(held.player1.right, &repeats[6], state->fps) || (state->settings->system.players >= 2 && repeat(held.player2.right, &repeats[7], state->fps)))
            {
                controls.right_pressed = 1;
            }
        }
    }

    return controls;
}

#define ERROR_BOX_WIDTH 300
#define ERROR_BOX_HEIGHT 50
#define ERROR_BOX_TOP 100

void display_test_error(state_t *state)
{
    unsigned int halfwidth = video_width() / 2;
    video_fill_box(
        halfwidth - (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP,
        halfwidth + (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP + ERROR_BOX_HEIGHT,
        rgb(32, 32, 32)
    );

    video_draw_line(
        halfwidth - (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP,
        halfwidth + (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP,
        rgb(255, 0, 0)
    );
    video_draw_line(
        halfwidth - (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP + ERROR_BOX_HEIGHT,
        halfwidth + (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP + ERROR_BOX_HEIGHT,
        rgb(255, 0, 0)
    );
    video_draw_line(
        halfwidth - (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP,
        halfwidth - (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP + ERROR_BOX_HEIGHT,
        rgb(255, 0, 0)
    );
    video_draw_line(
        halfwidth + (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP,
        halfwidth + (ERROR_BOX_WIDTH / 2),
        ERROR_BOX_TOP + ERROR_BOX_HEIGHT,
        rgb(255, 0, 0)
    );

    video_draw_text(
        halfwidth - (ERROR_BOX_WIDTH / 2) + 22,
        ERROR_BOX_TOP + 10,
        state->font_12pt,
        rgb(255, 0, 0),
        "Cannot edit menu settings on this screen!"
    );
    video_draw_text(
        halfwidth - (ERROR_BOX_WIDTH / 2) + 12,
        ERROR_BOX_TOP + 25,
        state->font_12pt,
        rgb(255, 0, 0),
        "Please edit settings from the main menu only!"
    );
}

unsigned int main_menu(state_t *state, int reinit)
{
    // Grab our configuration.
    static unsigned int count = 0;
    static games_list_t *games = 0;

    // Leave 24 pixels of padding on top and bottom of the games list.
    // Space out games 16 pixels across.
    static unsigned int maxgames = 0;

    // Where we are on the screen for both our cursor and scroll position.
    static unsigned int cursor = 0;
    static unsigned int top = 0;

    // Whether we're currently waiting to be rebooted for a game to send to us.
    static unsigned int controls_locked = 0;
    static unsigned int booting = 0;
    static double booting_animation = 0.0;
    static unsigned int holding = 0;
    static double holding_animation = 0.0;

    if (reinit)
    {
        games = get_games_list(&count);
        maxgames = (video_height() - (24 + 16)) / 21;
        cursor = selected_game;
        top = 0;
        if (cursor >= (top + maxgames))
        {
            top = cursor - (maxgames - 1);
        }
        controls_locked = 0;
        booting = 0;
        booting_animation = 0.0;
        holding = 0;
        holding_animation = 0.0;

        // Clear any error screens.
        state->test_error_counter = 0.0;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_MAIN_MENU;

    // Get our controls, including repeats.
    controls_t controls = get_controls(state, reinit);

    if (controls.test_pressed)
    {
        // Request to go into our configuration screen.
        if (booting == 0 && holding == 0)
        {
            selected_game = cursor;
            new_screen = SCREEN_CONFIGURATION;
        }
    }
    else
    {
        if (controls.start_pressed)
        {
            // Possibly long-pressing to get into game settings menu.
            if (!controls_locked)
            {
                controls_locked = 1;
                if (booting == 0 && holding == 0)
                {
                    holding = 1;
                    holding_animation = state->animation_counter;
                }
            }
        }
        if (controls.start_released)
        {
            if (booting == 0 && holding == 1)
            {
                // Made a selection!
                booting = 1;
                holding = 0;
                booting_animation = state->animation_counter;
                message_send(MESSAGE_SELECTION, &cursor, 4);
            }
            else if(booting == 1)
            {
                // Ignore everything, we're waiting to boot at this point.
            }
            else
            {
                // Somehow got here, maybe start held on another screen?
                booting = 0;
                holding = 0;
                controls_locked = 0;
            }
        }
        if (!controls_locked)
        {
            if (controls.up_pressed)
            {
                // Moved cursor up.
                if (cursor > 0)
                {
                    cursor --;
                }
                if (cursor < top)
                {
                    top = cursor;
                }
            }
            else if (controls.down_pressed)
            {
                // Moved cursor down.
                if (cursor < (count - 1))
                {
                    cursor ++;
                }
                if (cursor >= (top + maxgames))
                {
                    top = cursor - (maxgames - 1);
                }
            }
        }
    }

    // Now, render the actual list of games.
    {
        unsigned int scroll_indicator_move_amount[4] = { 1, 2, 1, 0 };
        int scroll_offset = scroll_indicator_move_amount[((int)(state->animation_counter * 4.0)) & 0x3];
        int cursor_offset = 0;

        if (holding > 0)
        {
            unsigned int cursor_move_amount[10] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
            unsigned int which = (int)((state->animation_counter - holding_animation) * 10.0);
            if (which >= 10)
            {
                // Held for 1 second, so lets go to game settings.
                selected_game = cursor;
                new_screen = SCREEN_GAME_SETTINGS_LOAD;
                which = 9;
            }
            cursor_offset = cursor_move_amount[which];
        }

        if (booting > 0)
        {
            if ((state->animation_counter - holding_animation) >= MAX_WAIT_FOR_COMMS)
            {
                // We failed to boot, display an error.
                new_screen = SCREEN_COMM_ERROR;
            }
        }

        if (top > 0)
        {
            video_draw_sprite(video_width() / 2 - 10, 10 - scroll_offset, up_png_width, up_png_height, up_png_data);
        }

        for (unsigned int game = top; game < top + maxgames; game++)
        {
            if (game >= count)
            {
                // Ran out of games to display.
                break;
            }

            // Draw cursor itself.
            if (game == cursor && (!booting))
            {
                video_draw_sprite(24 + cursor_offset, 24 + ((game - top) * 21), cursor_png_width, cursor_png_height, cursor_png_data);
            }

            unsigned int away = abs(game - cursor);
            int horizontal_offset = 0;
            if (away > 0 && booting > 0)
            {
                // How far behind should this animation play? this means that the animation plays in
                // waves starting at the cursor and fanning out.
                double x = ((state->animation_counter - booting_animation) * 1.25) - (((double)away) * 0.1);
                if (x <= 0)
                {
                    horizontal_offset = 0;
                }
                else
                {
                    // Reduce to half wave by 10 away from the cursor. This makes the animation less
                    // pronounced the further away it gets.
                    double coeff = -(900.0 - 450.0 * ((double)(away >= 10 ? 10 : away) / 10.0));

                    // Quadratic equation that puts the text in the same spot at 0.6 seconds into the
                    // animation, and has a maximum positive horizontal displacement of ~90 pixels.
                    // Of course this gets flattened the further out from the cursor you go, due to the
                    // above coeff calculation.
                    horizontal_offset = (int)((coeff * x) * (x - 0.6));
                }
            }

            // Draw game, highlighted if it is selected.
            video_draw_text(48 + horizontal_offset, 22 + ((game - top) * 21), state->font_18pt, game == cursor ? rgb(255, 255, 20) : rgb(255, 255, 255), games[game].name);
        }

        if ((top + maxgames) < count)
        {
            video_draw_sprite(video_width() / 2 - 10, 24 + (maxgames * 21) + scroll_offset, dn_png_width, dn_png_height, dn_png_data);
        }
    }

    return new_screen;
}

unsigned int game_settings_load(state_t *state, int reinit)
{
    static double load_start = 0.0;
    static unsigned int ack_received = 0;

    if (reinit)
    {
        // Attempt to fetch the game settings for this game.
        uint32_t which_game = selected_game;
        message_send(MESSAGE_LOAD_SETTINGS, &which_game, 4);
        load_start = state->animation_counter;
        ack_received = 0;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_GAME_SETTINGS_LOAD;

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    // Check to see if we got a response in time.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_LOAD_SETTINGS_ACK && length == 4)
            {
                uint32_t which_game;
                memcpy(&which_game, data, 4);

                if (which_game == selected_game)
                {
                    // Menu got our request, it should be gathering and sending settings
                    // to us at the moment.
                    ack_received = 1;
                }
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }

        if ((!ack_received) && ((state->animation_counter - load_start) >= MAX_WAIT_FOR_COMMS))
        {
            // Uh oh, no ack.
            new_screen = SCREEN_COMM_ERROR;
        }
    }

    video_draw_text(video_width() / 2 - 100, 100, state->font_18pt, rgb(0, 255, 0), "Fetching game settings...");

    return new_screen;
}

unsigned int game_settings(state_t *state, int reinit)
{
    if (reinit)
    {
        // TODO
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_GAME_SETTINGS;

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    video_draw_text(video_width() / 2 - 100, 100, state->font_18pt, rgb(255, 255, 0), "TODO...");

    return new_screen;
}

unsigned int comm_error(state_t *state, int reinit)
{
    if (reinit)
    {
        // Nothing to re-init, this screen is stuck. If we get here it means
        // the menu software on the other side is gone so there is no point in
        // trying to do anything.
    }

    controls_t controls = get_controls(state, reinit);
    if (controls.test_pressed)
    {
        // Display error message about not going into settings now.
        state->test_error_counter = state->animation_counter;
    }

    video_draw_text(video_width() / 2 - 50, 100, state->font_18pt, rgb(255, 0, 0), "Comm Error!");
    video_draw_text(
        video_width() / 2 - 130,
        130,
        state->font_12pt,
        rgb(255, 255, 255),
        "We seem to have lost communication with the\n"
        "controlling software! Cycle your cabinet power\n"
        "and run the menu software to try again!"
    );

    return SCREEN_COMM_ERROR;
}

unsigned int configuration(state_t *state, int reinit)
{
    static uint32_t options[7];
    static uint32_t maximums[7];
    static uint32_t lockable[7];
    static uint32_t disabled[7];
    static unsigned int cursor = 0;
    static unsigned int top = 0;
    static unsigned int maxoptions = 0;
    static int locked = -1;

    static uint8_t joy1_hcenter;
    static uint8_t joy1_vcenter;
    static uint8_t joy2_hcenter;
    static uint8_t joy2_vcenter;
    static uint8_t joy1_hmin;
    static uint8_t joy1_hmax;
    static uint8_t joy1_vmin;
    static uint8_t joy1_vmax;
    static uint8_t joy2_hmin;
    static uint8_t joy2_hmax;
    static uint8_t joy2_vmin;
    static uint8_t joy2_vmax;

    if (reinit)
    {
        options[0] = state->config->enable_analog;
        options[1] = state->config->system_region;
        options[2] = state->config->use_filenames;
        options[3] = 0;
        options[4] = 0;
        maximums[0] = 1;
        maximums[1] = 3;
        maximums[2] = 1;
        maximums[3] = 0;
        maximums[4] = 0;
        lockable[0] = 0;
        lockable[1] = 0;
        lockable[2] = 0;
        lockable[3] = 1;
        lockable[4] = 1;
        disabled[0] = 0;
        disabled[1] = 0;
        disabled[2] = 0;
        disabled[3] = 0;
        disabled[4] = state->settings->system.players == 1;

        // Dummy options for save and exit.
        options[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        options[((sizeof(options) / sizeof(options[0])) - 2)] = 0;
        maximums[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        maximums[((sizeof(options) / sizeof(options[0])) - 2)] = 0;
        lockable[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        lockable[((sizeof(options) / sizeof(options[0])) - 2)] = 0;
        disabled[((sizeof(options) / sizeof(options[0])) - 1)] = 0;
        disabled[((sizeof(options) / sizeof(options[0])) - 2)] = 0;

        // Calibration special case.
        joy1_hcenter = state->config->joy1_hcenter;
        joy1_vcenter = state->config->joy1_vcenter;
        joy2_hcenter = state->config->joy2_hcenter;
        joy2_vcenter = state->config->joy2_vcenter;
        joy1_hmin = state->config->joy1_hmin;
        joy1_hmax = state->config->joy1_hmax;
        joy1_vmin = state->config->joy1_vmin;
        joy1_vmax = state->config->joy1_vmax;
        joy2_hmin = state->config->joy2_hmin;
        joy2_hmax = state->config->joy2_hmax;
        joy2_vmin = state->config->joy2_vmin;
        joy2_vmax = state->config->joy2_vmax;

        cursor = 0;
        top = 0;
        maxoptions = (video_height() - (24 + 16 + 21 + 21)) / 21;
        locked = -1;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_CONFIGURATION;

    // Calculate disabled controls.
    if (options[0])
    {
        disabled[3] = 0;
        disabled[4] = state->settings->system.players == 1;
    }
    else
    {
        disabled[3] = 1;
        disabled[4] = 1;
    }

    // Get our controls, including repeats.
    controls_t controls = get_controls(state, reinit);

    if (controls.test_pressed)
    {
        // Test cycles as a safeguard.
        if (cursor == ((sizeof(options) / sizeof(options[0])) - 1))
        {
            // Exit without save.
            new_screen = SCREEN_MAIN_MENU;
        }
        else if (cursor == ((sizeof(options) / sizeof(options[0])) - 2))
        {
            // Exit with save.
            new_screen = SCREEN_MAIN_MENU;

            state->config->enable_analog = options[0];
            state->config->system_region = options[1];
            state->config->use_filenames = options[2];

            // Calibration special case.
            state->config->joy1_hcenter = joy1_hcenter;
            state->config->joy1_vcenter = joy1_vcenter;
            state->config->joy2_hcenter = joy2_hcenter;
            state->config->joy2_vcenter = joy2_vcenter;
            state->config->joy1_hmin = joy1_hmin;
            state->config->joy1_hmax = joy1_hmax;
            state->config->joy1_vmin = joy1_vmin;
            state->config->joy1_vmax = joy1_vmax;
            state->config->joy2_hmin = joy2_hmin;
            state->config->joy2_hmax = joy2_hmax;
            state->config->joy2_vmin = joy2_vmin;
            state->config->joy2_vmax = joy2_vmax;


            // Send back to PC.
            message_send(MESSAGE_SAVE_CONFIG, state->config, 64);
            new_screen = SCREEN_CONFIGURATION_SAVE;
        }
        else if (!disabled[cursor])
        {
            if (lockable[cursor])
            {
                if (cursor == locked)
                {
                    // Unlock control.
                    locked = -1;
                }
                else
                {
                    // Lock to this control.
                    locked = cursor;
                }
            }
            else if (locked == -1)
            {
                // Only edit controls if locking is diabled.
                if (options[cursor] < maximums[cursor])
                {
                    options[cursor]++;
                }
                else
                {
                    options[cursor] = 0;
                }
            }
        }
    }
    else if (controls.start_pressed)
    {
        // Test cycles as a safeguard.
        if (cursor == ((sizeof(options) / sizeof(options[0])) - 1))
        {
            // Exit without save.
            new_screen = SCREEN_MAIN_MENU;
        }
        else if (cursor == ((sizeof(options) / sizeof(options[0])) - 2))
        {
            // Exit with save.
            new_screen = SCREEN_MAIN_MENU;

            state->config->enable_analog = options[0];
            state->config->system_region = options[1];
            state->config->use_filenames = options[2];

            // Calibration special case.
            state->config->joy1_hcenter = joy1_hcenter;
            state->config->joy1_vcenter = joy1_vcenter;
            state->config->joy2_hcenter = joy2_hcenter;
            state->config->joy2_vcenter = joy2_vcenter;
            state->config->joy1_hmin = joy1_hmin;
            state->config->joy1_hmax = joy1_hmax;
            state->config->joy1_vmin = joy1_vmin;
            state->config->joy1_vmax = joy1_vmax;
            state->config->joy2_hmin = joy2_hmin;
            state->config->joy2_hmax = joy2_hmax;
            state->config->joy2_vmin = joy2_vmin;
            state->config->joy2_vmax = joy2_vmax;

            // Send back to PC.
            message_send(MESSAGE_SAVE_CONFIG, state->config, 64);
            new_screen = SCREEN_CONFIGURATION_SAVE;
        }
        else if (!disabled[cursor])
        {
            if (lockable[cursor])
            {
                if (cursor == locked)
                {
                    // Unlock control.
                    locked = -1;
                }
                else
                {
                    // Lock to this control.
                    locked = cursor;
                }
            }
        }
    }
    else if(locked == -1)
    {
        if(controls.up_pressed)
        {
            if (cursor > 0)
            {
                cursor--;
            }
        }
        else if(controls.down_pressed)
        {
            if (cursor < ((sizeof(options) / sizeof(options[0])) - 1))
            {
                cursor++;
            }
        }
        else if(controls.service_pressed)
        {
            // Service cycles as a safeguard.
            if (cursor < ((sizeof(options) / sizeof(options[0])) - 1))
            {
                cursor++;
            }
            else
            {
                cursor = 0;
            }
        }
        else if (!disabled[cursor])
        {
            if(controls.left_pressed)
            {
                if (options[cursor] > 0)
                {
                    options[cursor]--;
                }
            }
            else if(controls.right_pressed)
            {
                if (options[cursor] < maximums[cursor])
                {
                    options[cursor]++;
                }
            }
        }
    }

    if (locked == 3)
    {
        // 1P calibration.
        jvs_buttons_t held = maple_buttons_current();

        joy1_vcenter = held.player1.analog1;
        joy1_hcenter = held.player1.analog2;

        joy1_hmin = min(joy1_hmin, joy1_hcenter);
        joy1_hmax = max(joy1_hmax, joy1_hcenter);
        joy1_vmin = min(joy1_vmin, joy1_vcenter);
        joy1_vmax = max(joy1_vmax, joy1_vcenter);
    }
    else if (locked == 4)
    {
        // 2P calibration.
        jvs_buttons_t held = maple_buttons_current();

        joy2_vcenter = held.player2.analog1;
        joy2_hcenter = held.player2.analog2;

        joy2_hmin = min(joy2_hmin, joy2_hcenter);
        joy2_hmax = max(joy2_hmax, joy2_hcenter);
        joy2_vmin = min(joy2_vmin, joy2_vcenter);
        joy2_vmax = max(joy2_vmax, joy2_vcenter);
    }

    // Actually draw the menu
    {
        video_draw_text(video_width() / 2 - 70, 22, state->font_18pt, rgb(0, 255, 255), "Menu Configuration");

        for (unsigned int option = top; option < top + maxoptions; option++)
        {
            if (option >= (sizeof(options) / sizeof(options[0])))
            {
                // Ran out of options to display.
                break;
            }

            // Draw cursor itself.
            if (option == cursor && locked == -1)
            {
                video_draw_sprite(24, 24 + 21 + ((option - top) * 21), cursor_png_width, cursor_png_height, cursor_png_data);
            }

            // Draw option, highlighted if it is selected.
            char buffer[64];
            switch(option)
            {
                case 0:
                {
                    // Enable analog
                    sprintf(buffer, "Analog controls: %s", options[option] ? "enabled" : "disabled");
                    break;
                }
                case 1:
                {
                    // System region
                    char *regions[4] = {"japan", "usa", "export", "korea"};
                    sprintf(buffer, "Naomi region: %s*", regions[options[option]]);
                    break;
                }
                case 2:
                {
                    // Filename display
                    sprintf(buffer, "Game name display: %s*", options[option] ? "from filename" : "from ROM");
                    break;
                }
                case 3:
                {
                    if (locked == 3)
                    {
                        // 1P analog calibration
                        sprintf(
                            buffer,
                            "h: %02X, v: %02X, max: %02X %02X %02X %02X",
                            joy1_hcenter,
                            joy1_vcenter,
                            joy1_hmin,
                            joy1_hmax,
                            joy1_vmin,
                            joy1_vmax
                        );
                    }
                    else
                    {
                        strcpy(buffer, "Player 1 analog calibration");
                    }
                    break;
                }
                case 4:
                {
                    if (locked == 4)
                    {
                        // 2P analog calibration
                        sprintf(
                            buffer,
                            "h: %02X, v: %02X, max: %02X %02X %02X %02X",
                            joy2_hcenter,
                            joy2_vcenter,
                            joy2_hmin,
                            joy2_hmax,
                            joy2_vmin,
                            joy2_vmax
                        );
                    }
                    else
                    {
                        strcpy(buffer, "Player 2 analog calibration");
                    }
                    break;
                }
                case ((sizeof(options) / sizeof(options[0])) - 2):
                {
                    // Save and exit display
                    strcpy(buffer, "Save and exit");
                    break;
                }
                case ((sizeof(options) / sizeof(options[0])) - 1):
                {
                    // Save and exit display
                    strcpy(buffer, "Exit without save");
                    break;
                }
                default:
                {
                    // Uh oh??
                    strcpy(buffer, "WTF?");
                    break;
                }
            }

            video_draw_text(
                48,
                22 + 21 + ((option - top) * 21),
                state->font_18pt,
                disabled[option] ? rgb(128, 128, 128) : (option == cursor ? (cursor == locked ? rgb(0, 255, 0) : rgb(255, 255, 20)) : rgb(255, 255, 255)),
                buffer
            );
        }

        // Draw asterisk for some settings.
        video_draw_text(48, 22 + 21 + (maxoptions * 21), state->font_12pt, rgb(255, 255, 255), "Options marked with an asterisk (*) take effect only on the next boot.");
    }

    return new_screen;
}

unsigned int configuration_save(state_t *state, int reinit)
{
    static double load_start = 0.0;

    if (reinit)
    {
        // Attempt to fetch the game settings for this game.
        load_start = state->animation_counter;
    }

    // If we need to switch screens.
    unsigned int new_screen = SCREEN_CONFIGURATION_SAVE;

    // Check to see if we got a response in time.
    {
        uint16_t type = 0;
        uint8_t *data = 0;
        unsigned int length = 0;
        if (message_recv(&type, (void *)&data, &length) == 0)
        {
            if (type == MESSAGE_SAVE_CONFIG_ACK && length == 0)
            {
                // Successfully acknowledged, time to go back to main screen.
                new_screen = SCREEN_MAIN_MENU;
            }

            // Wipe any data that we need.
            if (data != 0)
            {
                free(data);
            }
        }

        if (((state->animation_counter - load_start) >= MAX_WAIT_FOR_SAVE))
        {
            // Uh oh, no ack.
            new_screen = SCREEN_COMM_ERROR;
        }
    }

    video_draw_text(video_width() / 2 - 100, 100, state->font_18pt, rgb(0, 255, 0), "Saving configuration...");

    return new_screen;
}

void main()
{
    // Grab the system configuration
    eeprom_t settings;
    eeprom_read(&settings);

    // Attach our communication handler for packet sending/receiving.
    packetlib_init();

    // Init the screen for a simple 640x480 framebuffer.
    video_init_simple();
    video_set_background_color(rgb(0, 0, 0));

    // Create global state for the menu.
    state_t state;
    state.settings = &settings;
    state.config = get_config();
    state.test_error_counter = 0.0;
    selected_game = state.config->boot_selection;

    // Attach our fonts
    state.font_18pt = video_font_add(dejavusans_ttf_data, dejavusans_ttf_len);
    video_font_set_size(state.font_18pt, 18);
    state.font_12pt = video_font_add(dejavusans_ttf_data, dejavusans_ttf_len);
    video_font_set_size(state.font_12pt, 12);

    // Add fallbacks if they are provided, for rendering CJK or other characters.
    unsigned int fallback_size;
    uint8_t *fallback_data = get_fallback_font(&fallback_size);
    if (fallback_size && fallback_data)
    {
        video_font_add_fallback(state.font_18pt, fallback_data, fallback_size);
        video_font_add_fallback(state.font_12pt, fallback_data, fallback_size);
    }

    // What screen we're on right now.
    unsigned int curscreen = SCREEN_MAIN_MENU;
    unsigned int oldscreen = -1;

    // FPS calculation for debugging.
    double fps_value = 60.0;

    // Simple animations for the screen.
    double animation_counter = 0.0;

    while ( 1 )
    {
        // Get FPS measurements.
        int fps = profile_start();
        int newscreen;

        // Set up the global state for any draw screen.
        state.fps = fps_value;
        state.animation_counter = animation_counter;

        // Now, draw the current screen.
        int profile = profile_start();
        switch(curscreen)
        {
            case SCREEN_MAIN_MENU:
                newscreen = main_menu(&state, curscreen != oldscreen);
                break;
            case SCREEN_GAME_SETTINGS_LOAD:
                newscreen = game_settings_load(&state, curscreen != oldscreen);
                break;
            case SCREEN_GAME_SETTINGS:
                newscreen = game_settings(&state, curscreen != oldscreen);
                break;
            case SCREEN_COMM_ERROR:
                newscreen = comm_error(&state, curscreen != oldscreen);
                break;
            case SCREEN_CONFIGURATION:
                newscreen = configuration(&state, curscreen != oldscreen);
                break;
            case SCREEN_CONFIGURATION_SAVE:
                newscreen = configuration_save(&state, curscreen != oldscreen);
                break;
            default:
                newscreen = curscreen;
                break;
        }

        if (state.test_error_counter > 0.0)
        {
            // Only display for 3 seconds.
            if ((state.animation_counter - state.test_error_counter) >= 3.0)
            {
                state.test_error_counter = 0.0;
            }
            else
            {
                display_test_error(&state);
            }
        }

        uint32_t draw_time = profile_end(profile);

        // Track what screen we are versus what we were so we know when we
        // switch screens.
        oldscreen = curscreen;
        curscreen = newscreen;

        if (state.config->enable_debug)
        {
            // Display some debugging info.
            video_draw_debug_text((video_width() / 2) - (18 * 4), video_height() - 16, rgb(0, 200, 255), "FPS: %.01f, %dx%d", fps_value, video_width(), video_height());
            video_draw_debug_text((video_width() / 2) - (18 * 4), video_height() - 24, rgb(0, 200, 255), "uS full draw: %d", draw_time);
        }

        // Actually draw the buffer.
        video_wait_for_vblank();
        video_display();

        uint32_t uspf = profile_end(fps);
        fps_value = (1000000.0 / (double)uspf) + 0.01;
        animation_counter += (double)uspf / 1000000.0;
    }
}

void test()
{
    // Initialize a simple console
    video_init_simple();
    video_set_background_color(rgb(0, 0, 0));

    while ( 1 )
    {
        // First, poll the buttons and act accordingly.
        maple_poll_buttons();
        jvs_buttons_t buttons = maple_buttons_pressed();

        if (buttons.psw1 || buttons.test)
        {
            // Request to go into system test mode.
            enter_test_mode();
        }

        // It would not make sense to have a test menu for our ROM. This is
        // because all of our settings are saved on the controlling PC or
        // Raspberry PI so that it can survive booting games and having the
        // EEPROM cleared every boot. So, nothing is worth changing here.
        video_draw_debug_text(
            (video_width() / 2) - (8 * (56 / 2)),
            (video_height() / 2) - (8 * 4),
            rgb(255, 255, 255),
            "No game settings available here. To change settings for\n"
            "the menu, press [test] when you are on the main screen.\n\n"
            "                  press [test] to exit                  "
        );
        video_wait_for_vblank();
        video_display();
    }
}

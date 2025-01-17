/*
 * Copyright (c) 2017, Matias Fontanini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following disclaimer
 *   in the documentation and/or other materials provided with the
 *   distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifdef _WIN32
    #define TINS_PREFIX_INTERFACE(x) ("\\Device\\NPF_" + x)
#else // _WIN32
    #define TINS_PREFIX_INTERFACE(x) (x)
#endif // _WIN32

#include <iostream>
#include <tins/sniffer.h>
#include <tins/dot11/dot11_base.h>
#include <tins/ethernetII.h>
#include <tins/radiotap.h>
#include <tins/loopback.h>
#include <tins/rawpdu.h>
#include <tins/dot3.h>
#include <tins/pktap.h>
#include <tins/sll.h>
#include <tins/ppi.h>
#include <tins/ip.h>
#include <tins/ipv6.h>
#include <tins/detail/pdu_helpers.h>

using std::string;

namespace Tins {

BaseSniffer::BaseSniffer() 
: handle_(0), mask_(0), extract_raw_(false) {
    
}
    
BaseSniffer::~BaseSniffer() {
    if (handle_) {
        pcap_close(handle_);
    }
}

void BaseSniffer::set_pcap_handle(pcap_t* pcap_handle) {
    handle_ = pcap_handle;
}

pcap_t* BaseSniffer::get_pcap_handle() {
    return handle_;
}

const pcap_t* BaseSniffer::get_pcap_handle() const {
    return handle_;
}

void BaseSniffer::set_if_mask(bpf_u_int32 if_mask) {
    mask_ = if_mask;
}

bpf_u_int32 BaseSniffer::get_if_mask() const {
    return mask_;
}

struct bpf_insn rt_pgm_crop_data[] = {
        // 00 LDB [3]      a = pkt[3] second half of length
        BPF_STMT(BPF_LD + BPF_MODE(BPF_ABS) + BPF_SIZE(BPF_B), 3),
        // 01 LSH #8       a = a << 8
        BPF_STMT(BPF_ALU + BPF_OP(BPF_LSH), 8),
        // 02 TAX          x = a
        BPF_STMT(BPF_MISC + BPF_MISCOP(BPF_TAX), 0),
        // 03 LDB [2]      a = pkt[2] first half of length
        BPF_STMT(BPF_LD + BPF_MODE(BPF_ABS) + BPF_SIZE(BPF_B), 2),
        // 04 ADD X         a = a + x  // combine endian swapped
        BPF_STMT(BPF_ALU + BPF_OP(BPF_ADD) + BPF_SRC(BPF_X), 0),

        // 05 ST M[0]      m[0] = a (rtap length)
        BPF_STMT(BPF_ST, 0),
        // 06 TAX          x = a = rtap length
        BPF_STMT(BPF_MISC + BPF_MISCOP(BPF_TAX), 0),

        // Fetch frame type

        // 07 LDB [x + 0]  a = pkt[rtap + 0]
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IND) + BPF_SIZE(BPF_B), 0),

        // 08 AND #0xC     a & 0xC - extract type
        BPF_STMT(BPF_ALU + BPF_OP(BPF_AND), 0xC),

        // 09 JEQ #0x0    if == 0, accept as management frame
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0x0, 0, 1),
        // 10 RET 0x40000  return success
        BPF_STMT(BPF_RET, 0x40000),

        // 11 JEQ #2       if a == 0x8, continue - process data frames (0x8 == b1000)
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0x8, 1, 0),
        // 12 RET 0x0      reject all other frames
        BPF_STMT(BPF_RET, 0),

        // 13 LDB [x + 0]  a = pkt[rtap + 0] - re-load A with FC byte
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IND) + BPF_SIZE(BPF_B), 0),
        // 14 AND #0xF0    a = a & 0xF0 - isolate subtype
        BPF_STMT(BPF_ALU + BPF_OP(BPF_AND), 0xF0),

        // 15 JEQ #0       a == 0x0 (subtype normal data)
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0x0, 0, 2),
        // 16 LD #24        a = 24 (non-qos header len)
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IMM), 24),
        // 17 JMP          jump past qos (22)
        BPF_STMT(BPF_JMP + BPF_JA, 3),

        // 18 JEQ #0x80    a == 0x80 (subtype qos data)
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0x80, 1, 0),
        // 29 RET 0x0      reject, data subtype we don't care to process
        BPF_STMT(BPF_RET, 0),

        // 20 LD #26       a = 26 - set qos header len
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IMM), 26),

        // 21 ST M[1]      m[1] = a (offset length) -- Store length before checking protected flag
        BPF_STMT(BPF_ST, 1),

        // 22 LDB [ x + 1]  a = pkt[rtap + 1] (flags)
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IND) + BPF_SIZE(BPF_B), 1),
        // 23 JSET 0x40   if a & 0x40 set protected bit
        BPF_JUMP(BPF_JMP + BPF_JSET, 0x40, 1, 0),

        // 24 LD #0       a = 0
        BPF_STMT(BPF_LD + BPF_SRC(BPF_K), 0),
        // 25 ST M[2]     m[2] = a
        BPF_STMT(BPF_ST, 2),

        // 26 LDA m[1] .   a = m[1] (saved data offset length)
        BPF_STMT(BPF_LD + BPF_MODE(BPF_MEM), 1),
        // 27 LDX M[0]     X = m[0] (rtap length)
        BPF_STMT(BPF_LDX + BPF_MODE(BPF_MEM), 0),
        // 28 ADD X        a = a + x
        BPF_STMT(BPF_ALU + BPF_OP(BPF_ADD) + BPF_SRC(BPF_X), 0),
        // 29 ST M[1]      m[1] = a (rtap length + offset length)
        BPF_STMT(BPF_ST, 1),
        // 30 TAX          x = a (x = total offset length)
        BPF_STMT(BPF_MISC + BPF_MISCOP(BPF_TAX), 0),

        // 31 LDA m[2] .   a = m[2] (saved flags)
        BPF_STMT(BPF_LD + BPF_MODE(BPF_MEM), 2),
        // 32 JEQ #0 .     a == 0x0 - truncate if it's a protected frame
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0x0, 0, 6),

        // 33 LDH [x + 0]  a = pkt[rtap + header + 0] - X hasn't been changed since we loaded it
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IND) + BPF_SIZE(BPF_H), 0),
        // 34 JEQ 0xAAAA   a == 0xAAAA (SNAP header) or truncate
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0xAAAA, 0, 4),

        // 35 LDB [x + 6]  a = pkt[rtap + header + 6]
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IND) + BPF_SIZE(BPF_B), 6),
        // 36 JEQ 0x88   a == 0x88 eapol sig or truncate
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0x88, 0, 2),

        // 37 LDB [x + 7]  a = pkt[rtap + header + 7]
        BPF_STMT(BPF_LD + BPF_MODE(BPF_IND) + BPF_SIZE(BPF_B), 7),
        // 38 JEQ 0x8e   a == 0x8e eapol sig or truncate
        BPF_JUMP(BPF_JMP + BPF_JEQ, 0x8E, 0, 2),

        // truncate - m1 holds the total length of the headers+qos

        // 39 LDB mem[1]
        BPF_STMT(BPF_LD + BPF_MODE(BPF_MEM), 1),
        // 40 RET a        Return a (limit packet length to rtap+dot11+qos?)
        BPF_STMT(BPF_RET + BPF_RVAL(BPF_A), 0),

        // 41 RET 0x0      return entire packet
        BPF_STMT(BPF_RET, 0x40000),
};
unsigned int rt_pgm_crop_data_len = 42;

struct sniff_data {
    struct timeval tv;
    PDU* pdu;
    bool packet_processed;

sniff_data() : tv(), pdu(0), packet_processed(true) { }
};

template<typename T>
T* safe_alloc(const u_char* bytes, bpf_u_int32 len) {
    try {
        return new T((const uint8_t*)bytes, len);
    }
    catch (malformed_packet&) {
        return 0;
    }
}

template<typename T>
void sniff_loop_handler(u_char* user, const struct pcap_pkthdr* h, const u_char* bytes) {
    sniff_data* data = (sniff_data*)user;
    data->packet_processed = true;
    data->tv = h->ts;
    data->pdu = safe_alloc<T>(bytes, h->caplen);
}

void sniff_loop_eth_handler(u_char* user, const struct pcap_pkthdr* h, const u_char* bytes) {
    sniff_data* data = (sniff_data*)user;
    data->packet_processed = true;
    data->tv = h->ts;
    if (Internals::is_dot3((const uint8_t*)bytes, h->caplen)) {
        data->pdu = safe_alloc<Dot3>((const uint8_t*)bytes, h->caplen);
    }
    else {
        data->pdu = safe_alloc<EthernetII>((const uint8_t*)bytes, h->caplen);
    }
}

void sniff_loop_raw_handler(u_char* user, const struct pcap_pkthdr* h, const u_char* bytes) {
    TINS_BEGIN_PACK
    struct base_ip_header {
    #if TINS_IS_LITTLE_ENDIAN
        uint8_t ihl:4,
                version:4;
    #else
        uint8_t version:4,
                ihl:4;
    #endif
    } TINS_END_PACK;

    sniff_data* data = (sniff_data*)user;
    const base_ip_header* header = (const base_ip_header*)bytes;
    data->packet_processed = true;
    data->tv = h->ts;
    switch (header->version) {
        case 4:
            data->pdu = safe_alloc<IP>((const uint8_t*)bytes, h->caplen);
            break;
        case 6:
            data->pdu = safe_alloc<IPv6>((const uint8_t*)bytes, h->caplen);
            break;
    };
}

#ifdef TINS_HAVE_DOT11
void sniff_loop_dot11_handler(u_char* user, const struct pcap_pkthdr* h, const u_char* bytes) {
    sniff_data* data = (sniff_data*)user;
    data->packet_processed = true;
    data->tv = h->ts;
    try {
        data->pdu = Dot11::from_bytes(bytes, h->caplen);
    }
    catch(malformed_packet&) {
        
    }
}
#endif

PtrPacket BaseSniffer::next_packet() {
    sniff_data data;
    const int iface_type = pcap_datalink(handle_);
    pcap_handler handler = 0;
    if (extract_raw_) {
        handler = &sniff_loop_handler<RawPDU>;
    }
    else {
        switch (iface_type) {
            case DLT_EN10MB:
                handler = &sniff_loop_eth_handler;
                break;
            case DLT_NULL:
                handler = &sniff_loop_handler<Tins::Loopback>;
                break;
            case DLT_LINUX_SLL:
                handler = &sniff_loop_handler<SLL>;
                break; 
            case DLT_PPI:
                handler = &sniff_loop_handler<PPI>;
                break;
            case DLT_RAW:
                handler = &sniff_loop_raw_handler;
                break;

            // Dot11 related protocols
            #ifdef TINS_HAVE_DOT11
            case DLT_IEEE802_11_RADIO:
                handler = &sniff_loop_handler<RadioTap>;
                break;
            case DLT_IEEE802_11:
                handler = &sniff_loop_dot11_handler;
                break;
            #else
            case DLT_IEEE802_11_RADIO:
            case DLT_IEEE802_11:
                throw protocol_disabled();
            #endif // TINS_HAVE_DOT11

            #ifdef DLT_PKTAP
            case DLT_PKTAP:
                handler = &sniff_loop_handler<PKTAP>;
                break;
            #endif // DLT_PKTAP

            default:
                throw unknown_link_type();
        }
    }
    // keep calling pcap_loop until a well-formed packet is found.
    while (data.pdu == 0 && data.packet_processed) {
        data.packet_processed = false;
        if (pcap_sniffing_method_(handle_, 1, handler, (u_char*)&data) < 0) {
            return PtrPacket(0, Timestamp());
        }
    }
    return PtrPacket(data.pdu, data.tv);
}

void BaseSniffer::set_extract_raw_pdus(bool value) {
    extract_raw_ = value;
}

void BaseSniffer::set_pcap_sniffing_method(PcapSniffingMethod method) {
    if (method == 0) {
        throw std::runtime_error("Sniffing method cannot be null");
    }
    pcap_sniffing_method_ = method;
}

void BaseSniffer::stop_sniff() {
    pcap_breakloop(handle_);
}

int BaseSniffer::get_fd() {
    #ifndef _WIN32
        return pcap_get_selectable_fd(handle_);
    #else
        throw unsupported_function();
    #endif // _WIN32
}

int BaseSniffer::link_type() const {
    return pcap_datalink(handle_);
}

BaseSniffer::iterator BaseSniffer::begin() {
    return iterator(this);
}

BaseSniffer::iterator BaseSniffer::end() {
    return iterator(0);
}

bool BaseSniffer::set_filter(const string& filter) {
    bpf_program prog;
    if (pcap_compile(handle_, &prog, filter.c_str(), 0, mask_) == -1) {
        return false;
    }
    
    struct bpf_program bpf{};
    bpf.bf_len = rt_pgm_crop_data_len;
    bpf.bf_insns = rt_pgm_crop_data;
    bool result = pcap_setfilter(handle_, &bpf) != -1;
    //pcap_freecode(&prog);
    std::cout << "filter applied" << std::endl;
    return result;
}

void BaseSniffer::set_timeout(int ms) {
    pcap_set_timeout(handle_, ms);
}

bool BaseSniffer::set_direction(pcap_direction_t d) {
	bool result = pcap_setdirection(handle_, d) != -1;
	return result;
}

// ****************************** Sniffer ******************************

Sniffer::Sniffer(const string& device) {
    init(device, SnifferConfiguration());
}

Sniffer::Sniffer(const string& device, const SnifferConfiguration& configuration) {
    init(device, configuration);
}

Sniffer::Sniffer(const string& device,
                 unsigned max_packet_size,
                 bool promisc, 
                 const string& filter,
                 bool rfmon) {
    SnifferConfiguration configuration;
    configuration.set_snap_len(max_packet_size);
    configuration.set_promisc_mode(promisc);
    configuration.set_filter(filter);
    configuration.set_rfmon(rfmon);

    init(device, configuration);
}

Sniffer::Sniffer(const string& device, 
                 promisc_type promisc,
                 const string& filter,
                 bool rfmon) {
    SnifferConfiguration configuration;
    configuration.set_promisc_mode(promisc == PROMISC);
    configuration.set_filter(filter);
    configuration.set_rfmon(rfmon);

    init(device, configuration);
}

void Sniffer::init(const string& device, const SnifferConfiguration& configuration) {
    char error[PCAP_ERRBUF_SIZE];
    pcap_t* phandle = pcap_create(TINS_PREFIX_INTERFACE(device).c_str(), error);
    if (!phandle) {
        throw pcap_error(error);
    }
    set_pcap_handle(phandle);

    // Set the netmask if we are able to find it.
    bpf_u_int32 ip, if_mask;
    if (pcap_lookupnet(TINS_PREFIX_INTERFACE(device).c_str(), &ip, &if_mask, error) == 0) {
        set_if_mask(if_mask);
    }

    // Configure the sniffer's attributes prior to activation.
    configuration.configure_sniffer_pre_activation(*this);

    // Finally, activate the pcap. In case of error, throw
    if (pcap_activate(get_pcap_handle()) < 0) {
        throw pcap_error(pcap_geterr(get_pcap_handle()));
    }

    // Configure the sniffer's attributes after activation.
    configuration.configure_sniffer_post_activation(*this);
}

void Sniffer::set_snap_len(unsigned snap_len) {
    if (pcap_set_snaplen(get_pcap_handle(), snap_len)) {
        throw pcap_error(pcap_geterr(get_pcap_handle()));
    }
}

void Sniffer::set_buffer_size(unsigned buffer_size) {
    if (pcap_set_buffer_size(get_pcap_handle(), buffer_size)) {
        throw pcap_error(pcap_geterr(get_pcap_handle()));
    }
}

void Sniffer::set_promisc_mode(bool promisc_enabled) {
    if (pcap_set_promisc(get_pcap_handle(), promisc_enabled)) {
        throw pcap_error(pcap_geterr(get_pcap_handle()));
    }
}

void Sniffer::set_immediate_mode(bool enabled) {
    // As of libpcap version 1.5.0 this function exists. Before, it was
    // technically always immediate mode since capture used TPACKET_V1/2
    // which doesn't do packet buffering.
    #ifdef HAVE_PCAP_IMMEDIATE_MODE
    if (pcap_set_immediate_mode(get_pcap_handle(), enabled)) {
        throw pcap_error(pcap_geterr(get_pcap_handle()));
    }
    #else
    Internals::unused(enabled);
    #endif // HAVE_PCAP_IMMEDIATE_MODE
}

void Sniffer::set_timestamp_precision(int value) {
    // This function exists as of libpcap version 1.5.0.
    #ifdef HAVE_PCAP_TIMESTAMP_PRECISION
    int result = pcap_set_tstamp_precision(get_pcap_handle(), value);
    if (result == PCAP_ERROR_TSTAMP_PRECISION_NOTSUP) {
        throw pcap_error("Timestamp precision not supported");
    }
    #else
    Internals::unused(value);
    #endif // HAVE_PCAP_TIMESTAMP_PRECISION
}

void Sniffer::set_rfmon(bool rfmon_enabled) {
    #ifndef _WIN32
    if (pcap_can_set_rfmon(get_pcap_handle()) == 1) {
        if (pcap_set_rfmon(get_pcap_handle(), rfmon_enabled)) {
            throw pcap_error(pcap_geterr(get_pcap_handle()));
        }
    }
    #endif
}


// **************************** FileSniffer ****************************

FileSniffer::FileSniffer(FILE *fp,
                         const SnifferConfiguration& configuration) {
    char error[PCAP_ERRBUF_SIZE];
    pcap_t* phandle = pcap_fopen_offline(fp, error);
    if (!phandle) {
        throw pcap_error(error);
    }
    set_pcap_handle(phandle);

    // Configure the sniffer
    configuration.configure_sniffer_pre_activation(*this);

}

FileSniffer::FileSniffer(const string& file_name, 
                         const SnifferConfiguration& configuration) {
    char error[PCAP_ERRBUF_SIZE];
    pcap_t* phandle = pcap_open_offline(file_name.c_str(), error);
    if (!phandle) {
        throw pcap_error(error);
    }
    set_pcap_handle(phandle);

    // Configure the sniffer
    configuration.configure_sniffer_pre_activation(*this);
    
}

FileSniffer::FileSniffer(const string& file_name, const string& filter) {
    SnifferConfiguration config;
    config.set_filter(filter);

    char error[PCAP_ERRBUF_SIZE];
    pcap_t* phandle = pcap_open_offline(file_name.c_str(), error);
    if (!phandle) {
        throw pcap_error(error);
    }
    set_pcap_handle(phandle);

    // Configure the sniffer
    config.configure_sniffer_pre_activation(*this);
}

FileSniffer::FileSniffer(FILE *fp, const string& filter) {
    SnifferConfiguration config;
    config.set_filter(filter);

    char error[PCAP_ERRBUF_SIZE];
    pcap_t* phandle = pcap_fopen_offline(fp, error);
    if (!phandle) {
        throw pcap_error(error);
    }
    set_pcap_handle(phandle);

    // Configure the sniffer
    config.configure_sniffer_pre_activation(*this);
}


// ************************ SnifferConfiguration ************************

const unsigned SnifferConfiguration::DEFAULT_SNAP_LEN = 65535;
const unsigned SnifferConfiguration::DEFAULT_TIMEOUT = 1000;

SnifferConfiguration::SnifferConfiguration()
: flags_(0), snap_len_(DEFAULT_SNAP_LEN), buffer_size_(0),
  pcap_sniffing_method_(pcap_loop), timeout_(DEFAULT_TIMEOUT), promisc_(false),
  rfmon_(false), immediate_mode_(false), direction_(PCAP_D_INOUT),
  timestamp_precision_(0) {

}

void SnifferConfiguration::configure_sniffer_pre_activation(Sniffer& sniffer) const {
    sniffer.set_snap_len(snap_len_);
    sniffer.set_timeout(timeout_);
    sniffer.set_pcap_sniffing_method(pcap_sniffing_method_);
    if ((flags_ & BUFFER_SIZE) != 0) {
        sniffer.set_buffer_size(buffer_size_);
    }
    if ((flags_ & PROMISCUOUS) != 0) {
        sniffer.set_promisc_mode(promisc_);
    }
    if ((flags_ & RFMON) != 0) {
        sniffer.set_rfmon(rfmon_);
    }
    if ((flags_ & IMMEDIATE_MODE) != 0) {
        sniffer.set_immediate_mode(immediate_mode_);
    }
    if ((flags_ & TIMESTAMP_PRECISION) != 0) {
        sniffer.set_timestamp_precision(timestamp_precision_);
    }
}

void SnifferConfiguration::configure_sniffer_pre_activation(FileSniffer& sniffer) const {
    if ((flags_ & PACKET_FILTER) != 0) {
        if (!sniffer.set_filter(filter_)) {
            throw invalid_pcap_filter(pcap_geterr(sniffer.get_pcap_handle()));
        }
    }
    sniffer.set_pcap_sniffing_method(pcap_sniffing_method_);
}

void SnifferConfiguration::configure_sniffer_post_activation(Sniffer& sniffer) const {
    if ((flags_ & PACKET_FILTER) != 0) {
        if (!sniffer.set_filter(filter_)) {
            throw invalid_pcap_filter(pcap_geterr(sniffer.get_pcap_handle()));
        }
    }
    // TODO: see how to actually do this on winpcap
    #ifndef _WIN32
    if ((flags_ & DIRECTION) != 0) {
        if (!sniffer.set_direction(direction_)) {
            throw pcap_error(pcap_geterr(sniffer.get_pcap_handle()));
        }
    }
    #endif // _WIN32
}

void SnifferConfiguration::set_snap_len(unsigned snap_len) {
    snap_len_ = snap_len;
}

void SnifferConfiguration::set_buffer_size(unsigned buffer_size) {
    flags_ |= BUFFER_SIZE;
    buffer_size_ = buffer_size;
}

void SnifferConfiguration::set_promisc_mode(bool enabled) {
    flags_ |= PROMISCUOUS;
    promisc_ = enabled;
}

void SnifferConfiguration::set_filter(const string& filter) {
    flags_ |= PACKET_FILTER;
    filter_ = filter;
}

void SnifferConfiguration::set_pcap_sniffing_method(BaseSniffer::PcapSniffingMethod method) {
    flags_ |= PCAP_SNIFFING_METHOD;
    pcap_sniffing_method_ = method;
}

void SnifferConfiguration::set_rfmon(bool enabled) {
    flags_ |= RFMON;
    rfmon_ = enabled;
}

void SnifferConfiguration::set_timeout(unsigned timeout) {
    timeout_ = timeout;
}

void SnifferConfiguration::set_immediate_mode(bool enabled) {
    flags_ |= IMMEDIATE_MODE;
    immediate_mode_ = enabled;
}

void SnifferConfiguration::set_timestamp_precision(int value) {
    flags_ |= TIMESTAMP_PRECISION;
    timestamp_precision_ = value;
}

void SnifferConfiguration::set_direction(pcap_direction_t direction) {
    direction_ =  direction;
    flags_ |= DIRECTION;
}

} // Tins

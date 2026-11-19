#ifdef FORZA_VORTEX_BACKEND
#include "selector.vortex.h"
#else

#include <cstring>
#include <functional>
#include "forza_message_lib.h"

#define PE_PER_ZAP 1  // Number of PEs per ZAP since we have SPMD model
#define MAX_PES 32
#define NMBOXES 1

namespace hclib {

    struct ForzaThreadArgs_ {
        uint32_t thread_type;
        uint32_t total_pes;
        uint32_t local_pes;
        uint32_t global_pe;  // single zone so same as local pe
        uint32_t zap;
        uint32_t zone;
        uint32_t local_pe;
        uint32_t n_mailboxes;
        uint32_t n_sends;
        uint32_t n_receives;
    };

    struct Packet56b {
        uint64_t v1;
        uint64_t v2;
        uint64_t v3;
        uint64_t v4;
        uint64_t v5;
        uint64_t v6;
        uint64_t v7;
    };

    //struct ForzaThreadArgs_ FArgs[2]; // one for each PE

    void* actor_thread_init( struct ForzaThreadArgs_* fargs ) {
        uint32_t recv_count = 0;
        uint32_t sum        = 0;
        uint32_t done       = 0;

        {
            char msg[50] = "\ndebug:PEX: actor thread init\n";
            msg[9]       = '0' + fargs->global_pe;
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
        }

        // Register this PE with ZQM
        uint32_t ret = forza_zqm_pe_setup( fargs->local_pe, fargs->n_mailboxes );
        if( ret ) {
            char msg[55] = "\ndebug:PEX: actor thread init: zqm setup failed\n";
            msg[9]       = '0' + fargs->global_pe;
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
            assert( 0 );  // Shouldn't fail in simulator
        } else {
            char msg[55] = "\ndebug:PEX: actor thread init: zqm setup succeeded\n";
            msg[9]       = '0' + fargs->global_pe;
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
        }

        // Wait for all threads to complete registrations
        forza_zone_barrier( forza_get_zaps_per_zone() );
        {
            char msg[55] = "\ndebug:PEX: actor thread init: after barrier1\n";
            msg[9]       = '0' + fargs->global_pe;
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
        }
        return NULL;
    }

    void forza_runtime_init() {
        uint64_t n_zaps      = forza_get_zaps_per_zone();
        uint64_t n_zones     = forza_get_zones_per_precinct();
        uint64_t n_precincts = forza_get_num_precincts();
        uint64_t n_local_pes = PE_PER_ZAP * n_zaps;  // PEs per zone
        uint64_t zap_id      = forza_get_my_zap();
        uint64_t zone_id     = forza_get_my_zone();
        uint64_t precinct_id = forza_get_my_precinct();

        if( n_zaps * n_zones * n_precincts > MAX_PES ) {
            char msg[50] = "\ndebug: System size exceeds MAX_PEs\n";
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
            assert( 0 );
        }

        {
            char msg[50] = "\ndebug:ZAPX: ZONEX: main: Enter main\n";
            msg[10]      = '0' + zap_id;
            msg[17]      = '0' + zone_id;
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
        }
        {
            char msg[55] = "\ndebug:ZAPX: main: n_zones=W, n_zaps=X, n_local_pes=Y\n";
            msg[27]      = '0' + n_zones;
            msg[37]      = '0' + n_zaps;
            msg[52]      = '0' + n_local_pes;
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
        }

        // Create local actor threads (PEs)
        // Currently function call, can also use forza_thread_create
        struct ForzaThreadArgs_ FArgs[MAX_PES];  // one for each PE
        // Currently spawning only one thread for each zap
        for( uint32_t j = 0; j < PE_PER_ZAP; j++ ) {  // Each thread at a zap will create the local thread(s)
            //index = Zone * PE/ZONE + ZAP * PE/ZAP;
            //PE/ZONE = PE/ZAP * ZAP/ZONE;
            uint32_t index           = zone_id * n_zaps * PE_PER_ZAP + zap_id * PE_PER_ZAP + j;
            FArgs[index].thread_type = 0;
            FArgs[index].total_pes   = PE_PER_ZAP * n_zaps * n_zones;
            FArgs[index].local_pes   = n_local_pes;
            FArgs[index].global_pe   = index;
            FArgs[index].zap         = zap_id;
            FArgs[index].zone        = zone_id;
            FArgs[index].local_pe    = index % n_local_pes;  // PE within zone
            FArgs[index].n_mailboxes = NMBOXES;
            //FArgs[index].n_sends     = NSENDS;

            {
                char msg[55] = "\ndebug:Px:ZONEx:ZAPx:LPEx main: index=y\n";
                msg[8]       = '0' + precinct_id;
                msg[14]      = '0' + zone_id;
                msg[19]      = '0' + zap_id;
                msg[24]      = '0' + FArgs[index].local_pe;
                msg[38]      = '0' + index;
                rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
            }
            actor_thread_init( &( FArgs[index] ) );
        }
        {
            char msg[30] = "\ndebug:ZAPX: main: complete\n";
            msg[10]      = '0' + zap_id;
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
        }
    }

    template<typename T>
        inline void launch( const char** deps, int ndeps, T&& lambda ) {
        {
            char msg[30] = "\ndebug: launch\n";
            rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
        }
        forza_runtime_init();
        lambda();
    }

    // check this later
    uint64_t forza_n_pes() {
        return PE_PER_ZAP * forza_get_zaps_per_zone() * forza_get_zones_per_precinct() * forza_get_num_precincts();
    }

    // check this later
    uint64_t forza_my_pe() {
        return forza_get_my_zone() * forza_get_zaps_per_zone() * PE_PER_ZAP + forza_get_my_zap() * PE_PER_ZAP + 0;
    }

    void forza_get_physical_ids_by_global_pe(uint64_t global_pe, uint64_t *precinct, uint64_t *zone, uint64_t *zone_local_pe) {
        uint64_t numPrecincts = forza_get_num_precincts();
        uint64_t zonesPerPrecinct = forza_get_zones_per_precinct();
        uint64_t zapsPerZone = forza_get_zaps_per_zone();
        uint64_t pePerZap = PE_PER_ZAP;

        // Calculate the total number of threads per zone
        uint64_t pesPerZone = zapsPerZone * pePerZap;

        // Calculate the number of PEs per precinct
        uint64_t pesPerPrecinct = zonesPerPrecinct * pesPerZone;

        // Calculate the IDs
        *precinct = global_pe / pesPerPrecinct;
        uint64_t remainder = global_pe % pesPerPrecinct;
        *zone = remainder / pesPerZone;
        remainder %= pesPerZone;
        //*zap = remainder / pePerZap;
        *zone_local_pe = remainder % pesPerZone;
    }

    template<typename T>
        inline void finish( T&& lambda ) {
        {
            char msg[60] = "\ndebug:PEX: reached finish\n";
            msg[9] = '0' + forza_my_pe();
            rev_write(STDOUT_FILENO, msg, sizeof(msg));
        }
        lambda();
    }

    template<typename T>
        class Mailbox {
    private:
        uint64_t doneReceived = 0;
    public:
        uint64_t mb_id;
        std::function<void (T, int)> process;

        Mailbox() {
        }

        bool send(T pkt, int rank) {
            uint64_t precinct, zone, zone_local_pe;

            // send
            forza_get_physical_ids_by_global_pe(rank, &precinct, &zone, &zone_local_pe);
            uint32_t sendstat = forza_send(mb_id, (T *) &pkt, precinct, zone, zone_local_pe);
            if (sendstat != SUCCESS) {
                {
                    char msg[60] = "\ndebug:PEX: send to MBY on PEZ failed\n";
                    msg[9] = '0' + forza_my_pe();
                    msg[22] = '0' + mb_id;
                    msg[29] = '0' + rank;
                    rev_write(STDOUT_FILENO, msg, sizeof(msg));
                }
                assert(0);
            } else {
                {
                    char msg[60] = "\ndebug:PEX: send to MBY on PEZ succeeded\n";
                    msg[9] = '0' + forza_my_pe();
                    msg[22] = '0' + mb_id;
                    msg[29] = '0' + rank;
                    rev_write(STDOUT_FILENO, msg, sizeof(msg));
                }
            }

            // switches to receive if # of outstanding messages exceed certain threshold
            receiveUntil(0);
            return true;
        }

        void safeCopyPacket56btoT(const Packet56b& packet, T& target) {
            size_t PacketSize = sizeof(packet);
            size_t TargetSize = sizeof(target);

            if (TargetSize > PacketSize) {
                char msg[128] = "\ndebug:PEX: safeCopyPacket56b PacketSize is bigger than 56b\n";
                msg[9]       = '0' + forza_my_pe();
                rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                assert( 0 );
            }
            if (!std::is_trivially_copyable_v<T>) {
                char msg[128] = "\ndebug:PEX: PacketType must be trivially copyable.\n";
                msg[9]       = '0' + forza_my_pe();
                rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                assert( 0 );
            }

            memcpy(&target, &packet, TargetSize);
        }

        void receiveUntil(uint64_t threshold) {
            uint64_t mcount;
            do {
                uint32_t msgstat = forza_get_outstanding_msg_count(mb_id, &mcount);
                if (msgstat != SUCCESS) {
                    {
                        char msg[60] = "\ndebug:PEX:MBY: getting outstanding msg count failed\n";
                        msg[9] = '0' + forza_my_pe();
                        msg[13] = '0' + mb_id;
                        rev_write(STDOUT_FILENO, msg, sizeof(msg));
                    }
                    assert(0);
                } else {
                    {
                        char msg[60] = "\ndebug:PEX:MBY: outstanding msgs = Z -> do receive\n";
                        msg[9] = '0' + forza_my_pe();
                        msg[13] = '0' + mb_id;
                        msg[35] = '0' + (uint32_t)mcount;
                        rev_write(STDOUT_FILENO, msg, sizeof(msg));
                    }
                    receive(false);
                }
            } while (mcount != threshold);
        }

        uint32_t receive(bool returnOnDone) {
            if (isDone()) { return DONE_MSG; }
            struct Packet56b rcv_pkt = { 0, 0, 0, 0, 0, 0, 0 };
            do {
                // If returnOnDone == false: return
                // Otherwise: keep receiving until done
                int msg_avail = forza_message_available(mb_id);
                if (!returnOnDone && msg_avail == NO_MSG) {
                    return NO_MSG;
                } else {
                    while (msg_avail == NO_MSG) {
                        msg_avail = forza_message_available(mb_id);
                    }
                }
                assert(msg_avail != NO_MSG);
                // Get message
                int ret = forza_message_receive(mb_id, (uint64_t *)&rcv_pkt, 7 * sizeof( uint64_t ));
                if (ret == SUCCESS) {
                    {
                        char msg[60] = "\ndebug:PEX: received msg\n";
                        msg[9]       = '0' + forza_my_pe();
                        rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                    }
                    // process message
                    T pkt;
                    safeCopyPacket56btoT(rcv_pkt, pkt);
                    process(pkt, -1);
                } else if (ret == DONE_MSG) {
                    {
                        char msg[60] = "\ndebug:PEX:MBY:receive:DONE msg doneReceived Z -> W\n";
                        msg[9]       = '0' + forza_my_pe();
                        msg[13]      = '0' + mb_id;
                        msg[45]      = '0' + (int32_t)doneReceived;
                        msg[50]      = '0' + (int32_t)doneReceived+1;
                        rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                    }
                    doneReceived++;
                } else {  // Invalid return status
                    char msg[65] = "\ndebug:PEX:MBY:message receive error\n";
                    msg[9] = '0' + forza_my_pe();
                    msg[13] = '0' + mb_id;
                    rev_write(STDOUT_FILENO, msg, sizeof(msg));
                    assert(0);
                }
            } while (!isDone());
            {
                char msg[60] = "\ndebug:PEX:MBY Exiting from DONE\n";
                msg[9]       = '0' + forza_my_pe();
                msg[13]       = '0' + mb_id;
                rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
            }
            return DONE_MSG;
        }

        bool isDone() {
            return (doneReceived == forza_n_pes());
        }

        void done() {
            for (uint64_t destPE = 0; destPE < forza_n_pes(); destPE++) {
                receiveUntil(0);
                uint64_t precinct, zone, zone_local_pe;
                {
                    char msg[60] = "\ndebug:PEX:MBY: sends done to PEZ\n";
                    msg[9]       = '0' + forza_my_pe();
                    msg[13]       = '0' + mb_id;
                    msg[32]      = '0' + destPE;
                    rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                }
                forza_get_physical_ids_by_global_pe(destPE, &precinct, &zone, &zone_local_pe);
                int sendstat = forza_send_done( mb_id, precinct, zone, zone_local_pe);
                if( sendstat != SUCCESS ) {
                    char msg[65] = "\ndebug:PEX:MBY actor send done to PEZ failed\n";
                    msg[9]       = '0' + forza_my_pe();
                    msg[13]      = '0' + mb_id;
                    msg[36]      = '0' + destPE;
                    rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                } else {
                    char msg[65] = "\ndebug:PEX:MBY actor send done to PEY succeeded\n";
                    msg[9]       = '0' + forza_my_pe();
                    msg[13]      = '0' + mb_id;
                    msg[36]      = '0' + destPE;
                    rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                }
            }

            {
                char msg[65] = "\ndebug:PEX:MBY actor send done broadcasted doneReceived=Z\n";
                msg[9]       = '0' + forza_my_pe();
                msg[13]      = '0' + mb_id;
                msg[57]      = '0' + doneReceived;
                rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
            }

            uint32_t recvStatus = NO_MSG;
            do {
                {
                    char msg[65] = "\ndebug:PEX:MBY extended_barrier PROGRESS (doneReceived=Y)\n";
                    msg[9]       = '0' + forza_my_pe();
                    msg[13]       = '0' + mb_id;
                    msg[55]      = '0' + doneReceived;
                    rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
                }
                recvStatus = receive(true);
            } while (recvStatus != DONE_MSG);
            //forza_zone_barrier( forza_get_zaps_per_zone() );
            {
                char msg[55] = "\ndebug:PEX:MBY extended_barrier COMPLETE\n";
                msg[9]       = '0' + forza_my_pe();
                msg[13]       = '0' + mb_id;
                rev_write( STDOUT_FILENO, msg, sizeof( msg ) );
            }
        }
    };

    template<int N, typename T=int64_t>
        class Selector {
    public:
    Mailbox<T> mb[N];

    Selector() {
        for (int i = 0; i < N; i++) {
            mb[i].mb_id = i;
        }
        {
            char msg[60] = "\ndebug:PEX: reached Selector Init\n";
            msg[9] = '0' + forza_my_pe();
            rev_write(STDOUT_FILENO, msg, sizeof(msg));
        }
    }

    void start() {
    }

    bool send(int mb_id, T pkt, int rank) {
        return mb[mb_id].send(pkt, rank);
    }

    void done(int mb_id = 0) {
        assert(mb_id == 0);
        {
            char msg[60] = "\ndebug:PEX: reached Selector Done\n";
            msg[9] = '0' + forza_my_pe();
            rev_write(STDOUT_FILENO, msg, sizeof(msg));
        }
        if (mb_id == 0) {
            for (int m = 0; m < N; m++) {
                mb[m].done();
                {
                    char msg[60] = "\ndebug:PEX:MBY DONE COMPLETE\n";
                    msg[9] = '0' + forza_my_pe();
                    msg[13] = '0' + m;
                    rev_write(STDOUT_FILENO, msg, sizeof(msg));
                }
            }
        }
    }
    };
}  // namespace hclib

#endif

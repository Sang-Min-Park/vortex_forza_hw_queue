#pragma once
#include "device.h"
#include <forza_mmio_abi.h>
namespace transport {
static inline uint32_t local(){return vx_warp_id()*VX_CFG_NUM_THREADS+vx_thread_id();}
static inline uint64_t endpoint(uint64_t e){return e+uint64_t(local())*FORZA_MMIO_ACTOR_STRIDE;}
static inline uint64_t load(uint64_t address){return *reinterpret_cast<volatile const uint64_t*>(address);}
static inline bool init(uint32_t mask){return swap64(reinterpret_cast<uint64_t*>(endpoint(FORZA_MMIO_REGISTER)),(uint64_t(local())<<8)|mask)==FORZA_MMIO_ACCEPTED;}
static inline uint64_t send(uint32_t core,uint32_t pe,uint32_t mailbox,uint64_t payload){uint64_t address=FORZA_MMIO_BASE+(uint64_t(core)<<17)+(uint64_t(pe)<<6)+(uint64_t(mailbox)<<3);return swap64(reinterpret_cast<uint64_t*>(address),payload);}
// The bulk descriptor ABI reserves payload zero for "no descriptor".
static inline uint64_t pop_descriptor(){return swap64(reinterpret_cast<uint64_t*>(endpoint(forza_mmio_pop_address(0))),0);}
static inline bool receive(uint32_t mailbox,uint64_t&payload,uint64_t&error){uint64_t pointer=load(endpoint(forza_mmio_peek_address(mailbox)));if(!pointer)return false;auto p=reinterpret_cast<volatile const uint64_t*>(pointer);uint64_t control=p[0];payload=p[1];uint64_t status=swap64(reinterpret_cast<uint64_t*>(endpoint(forza_mmio_release_address(mailbox))),control^payload);if(status!=0)error=10;return true;}
static inline uint64_t credits(){return load(endpoint(FORZA_MMIO_TX_COUNTS));}
static inline bool healthy(){uint64_t tx=load(endpoint(FORZA_MMIO_TX_STATUS)),rx=load(endpoint(FORZA_MMIO_RX_STATUS));return(tx>>63)&&!(tx&(UINT64_C(0x1ff)<<8))&&!(rx&(UINT64_C(255)<<8));}
}

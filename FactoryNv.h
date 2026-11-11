// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read current factory NV from this Pong's actual LUN5/GPT. The exported
// interface accepts only four named partitions, never an arbitrary LBA.
// Reads share the existing UFS DMA owner/lock. Writes remain restricted
// to the original LUN0 SHNGROOT windows; no factory-NV write path exists.
struct FactoryNvPartition {uint64_t first=0,blocks=0;};
static FactoryNvPartition gFactoryNv[4];
static bool gFactoryNvReady;
static uint8_t gFactoryNvScratch[16384];
static const char *const gFactoryNvNames[]={"modemst1","modemst2","fsg","fsc"};

static bool factoryNvName(const uint8_t *entry,const char *name) {
  unsigned n=0;
  for(;name[n];n++)if(n>=36||entry[56+2*n]!=uint8_t(name[n])||entry[57+2*n])return false;
  for(;n<36;n++)if(entry[56+2*n]||entry[57+2*n])return false;
  return true;
}
static int factoryNvDiscoverLocked() {
  if(gFactoryNvReady)return 0;
  if(!gHost.ready||gHost.poisoned)return EAGAIN;
  uint32_t scsi=0;bool ready=false;
  const uint8_t tur[6]={};
  for(unsigned attempt=0;attempt<3&&!gHost.poisoned;attempt++) {
    if(hostScsi(kPongFactoryNvLun,tur,sizeof tur,false,0,200000,&scsi)==kIOReturnSuccess){ready=true;break;}
    IOSleep(10);
  }
  if(!ready)return EIO;
  const uint8_t capacity[10]={0x25};
  if(hostScsi(kPongFactoryNvLun,capacity,sizeof capacity,true,8,200000,&scsi)!=kIOReturnSuccess)return EIO;
  const uint64_t blocks=uint64_t(loadBe32(gHost.virt+kDataOffset))+1;
  if(blocks!=kPongFactoryNvBlocks4k||loadBe32(gHost.virt+kDataOffset+4)!=4096)return ENODEV;
  gHost.factoryBlocks=blocks;
  if(hostRead4kLocked(kPongFactoryNvLun,1,1)!=kIOReturnSuccess)return EIO;
  memcpy(gFactoryNvScratch,gHost.virt+kDataOffset,4096);
  const auto *header=gFactoryNvScratch;
  const uint32_t length=le32(header+12),count=le32(header+80),entrySize=le32(header+84);
  if(memcmp(header,"EFI PART",8)||le32(header+8)!=0x10000||length<92||length>4096||
     le32(header+20)||le64(header+24)!=1||le64(header+32)!=blocks-1||
     rootCrc32(header,length,true)!=le32(header+16)||!count||count>128||entrySize!=128)return EINVAL;
  const uint64_t entries=le64(header+72),first=le64(header+40),last=le64(header+48);
  const uint32_t arrayCrc=le32(header+88),arrayBlocks=(count*128+4095)/4096;
  if(entries<2||entries>64||entries+arrayBlocks>first||first>last||last>=blocks-1-arrayBlocks)return EINVAL;
  if(hostRead4kLocked(kPongFactoryNvLun,entries,arrayBlocks)!=kIOReturnSuccess)return EIO;
  memcpy(gFactoryNvScratch,gHost.virt+kDataOffset,arrayBlocks*4096);
  if(rootCrc32(gFactoryNvScratch,count*128)!=arrayCrc)return EINVAL;
  FactoryNvPartition found[4]{};
  for(unsigned i=0;i<count;i++) {
    const auto *entry=gFactoryNvScratch+i*128;if(!rootEntryUsed(entry))continue;
    for(unsigned part=0;part<4;part++)if(factoryNvName(entry,gFactoryNvNames[part])) {
      const uint64_t lo=le64(entry+32),hi=le64(entry+40);
      if(found[part].blocks||lo<first||hi<lo||hi>last)return EINVAL;
      const uint64_t size=hi-lo+1;
      // Exact actual Pong geometry from REC, not a guessed modem CMA.
      if(size!=(part==3?32u:768u))return ENODEV;
      found[part]={lo,size};
    }
  }
  for(unsigned i=0;i<4;i++) {
    if(!found[i].blocks)return ENOENT;
    for(unsigned j=0;j<i;j++)
      if(found[i].first<found[j].first+found[j].blocks&&found[j].first<found[i].first+found[i].blocks)return EINVAL;
  }
  memcpy(gFactoryNv,found,sizeof found);gFactoryNvReady=true;
  IOLog("PONG_UFS: actual LUN5 factory NV GPT ready; four original partitions read-only, no snapshots or NV writes\n");
  return 0;
}
static int factoryNvRead(unsigned slot,uint64_t offset,void *destination,uint32_t bytes,uint64_t *total) {
  if(slot>=4||!total||(bytes&&!destination)||!gHost.lock)return EINVAL;
  *total=0;IOLockLock(gHost.lock);
  int result=factoryNvDiscoverLocked();
  if(!result) {
    const auto &part=gFactoryNv[slot];const uint64_t size=part.blocks*4096;
    *total=size;
    if(offset>size||bytes>size-offset)result=EINVAL;
    auto *out=static_cast<uint8_t *>(destination);uint32_t left=bytes;
    while(!result&&left) {
      const uint32_t within=uint32_t(offset&4095);
      const uint32_t copy=left<kDataMax-within?left:uint32_t(kDataMax-within);
      const uint32_t n=(within+copy+4095)/4096;
      if(hostRead4kLocked(kPongFactoryNvLun,part.first+offset/4096,n)!=kIOReturnSuccess){result=EIO;break;}
      memcpy(out,gHost.virt+kDataOffset+within,copy);
      out+=copy;offset+=copy;left-=copy;
      // Let APFS and other legitimate storage clients make progress
      // between completed bounded DMA transfers, never during ownership.
      if(left){IOLockUnlock(gHost.lock);IOLockLock(gHost.lock);}
    }
  }
  IOLockUnlock(gHost.lock);return result;
}

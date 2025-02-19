#define ull unsigned long long
#define MAX (64*1024*1024)
#define WARPSIZE 32

/*
DPCT1078:0: Consider replacing memory_order::acq_rel with
memory_order::seq_cst for correctness if strong memory order restrictions
are needed.
*/
#define __threadfence_block() \
  sycl::atomic_fence(sycl::memory_order::acq_rel,sycl::memory_scope::work_group)

/*
This is the GPU compression kernel, which should be launched using the block count
and warps/block:
  CompressionKernel<<<blocks, WARPSIZE*warpsperblock>>>();

Inputs
------
dimensionalityd: dimensionality of trace (from cmd line)
cbufd: ptr to the uncompressed data
cutd: ptr to array of chunk boundaries

Output
------
The compressed data, in dbufd 
Compressed chunk offsets for offset table, in offd
*/

void CompressionKernel(
  nd_item<1> item,
  const int dimensionalityd,
  const ull *__restrict cbufd,
  char *__restrict dbufd,
  const int *__restrict cutd,
  int *__restrict offd,
  volatile int *__restrict ibufs)
{
  int offset, code, bcount, tmp, off, beg, end, lane, warp, iindex, lastidx, start, term;
  ull diff, prev;
   // shared space for prefix sum

  const int lid = item.get_local_id(0);

  // index within this warp
  lane = lid & 31;
  // index within shared prefix sum array
  iindex = lid / WARPSIZE * (3 * WARPSIZE / 2) + lane;
  ibufs[iindex] = 0;
  iindex += WARPSIZE / 2;
  lastidx = (lid / WARPSIZE + 1) * (3 * WARPSIZE / 2) - 1;
  // warp id
  warp = item.get_global_id(0) / WARPSIZE;

  // prediction index within previous subchunk
  offset = WARPSIZE - (dimensionalityd - lane % dimensionalityd) - lane;

  // determine start and end of chunk to compress
  start = 0;
  if (warp > 0) start = cutd[warp-1];
  term = cutd[warp];
  off = ((start+1)/2*17);

  prev = 0;
  for (int i = start + lane; i < term; i += WARPSIZE) {
    // calculate delta between value to compress and prediction
    // and negate if negative
    diff = cbufd[i] - prev;
    code = (diff >> 60) & 8;
    if (code != 0) {
      diff = -diff;
    }

    // count leading zeros in positive delta
    bcount = 8 - (sycl::clz(diff) >> 3);
    if (bcount == 2) bcount = 3; // encode 6 lead-zero bytes as 5

    // prefix sum to determine start positions of non-zero delta bytes
    ibufs[iindex] = bcount;
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-1];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-2];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-4];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-8];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-16];
    __threadfence_block();

    // write out non-zero bytes of delta to compressed buffer
    beg = off + (WARPSIZE/2) + ibufs[iindex-1];
    end = beg + bcount;
    for (; beg < end; beg++) {
      dbufd[beg] = diff;
      diff >>= 8;
    }

    if (bcount >= 3) bcount--; // adjust byte count for the dropped encoding
    tmp = ibufs[lastidx];
    code |= bcount;
    ibufs[iindex] = code;

    __threadfence_block();

    // write out half-bytes of sign and leading-zero-byte count (every other thread
    // writes its half-byte and neighbor's half-byte)
    if ((lane & 1) != 0) {
      dbufd[off + (lane >> 1)] = ibufs[iindex-1] | (code << 4);
    }
    off += tmp + (WARPSIZE/2);

    // save prediction value from this subchunk (based on provided dimensionality)
    // for use in next subchunk
    prev = cbufd[i + offset];
  }

  // save final value of off, which is total bytes of compressed output for this chunk
  if (lane == 31) offd[warp] = off;
}

/************************************************************************************/

/*
This is the GPU decompression kernel, which should be launched using the block count
and warps/block:
  CompressionKernel<<<blocks, WARPSIZE*warpsperblock>>>();

Inputs
------
dimensionalityd: dimensionality of trace
dbufd: ptr to array of compressed data
cutd: ptr to array of chunk boundaries

Output
------
The decompressed data in fbufd
*/

void DecompressionKernel(
 nd_item<1> item,
 const int dimensionalityd,
 const char *__restrict dbufd,
 ull *__restrict fbufd,
 const int *__restrict cutd,
 volatile int *__restrict ibufs)
{
  int offset, code, bcount, off, beg, end, lane, warp, iindex, lastidx, start, term;
  ull diff, prev;
  const int lid = item.get_local_id(0);

  // index within this warp
  lane = lid & 31;
  // index within shared prefix sum array
  iindex = lid / WARPSIZE * (3 * WARPSIZE / 2) + lane;
  ibufs[iindex] = 0;
  iindex += WARPSIZE / 2;
  lastidx = (lid / WARPSIZE + 1) * (3 * WARPSIZE / 2) - 1;
  // warp id
  warp = item.get_global_id(0) / WARPSIZE;
  // prediction index within previous subchunk
  offset = WARPSIZE - (dimensionalityd - lane % dimensionalityd) - lane;

  // determine start and end of chunk to decompress
  start = 0;
  if (warp > 0) start = cutd[warp-1];
  term = cutd[warp];
  off = ((start+1)/2*17);

  prev = 0;
  for (int i = start + lane; i < term; i += WARPSIZE) {
    // read in half-bytes of size and leading-zero count information
    if ((lane & 1) == 0) {
      code = dbufd[off + (lane >> 1)];
      ibufs[iindex] = code;
      ibufs[iindex + 1] = code >> 4;
    }
    off += (WARPSIZE/2);
    __threadfence_block();

    code = ibufs[iindex];

    bcount = code & 7;
    if (bcount >= 2) bcount++;

    // calculate start positions of compressed data
    ibufs[iindex] = bcount;
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-1];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-2];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-4];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-8];
    __threadfence_block();

    ibufs[iindex] += ibufs[iindex-16];
    __threadfence_block();

    // read in compressed data (the non-zero bytes)
    beg = off + ibufs[iindex-1];
    off += ibufs[lastidx];
    end = beg + bcount - 1;
    diff = 0;
    for (; beg <= end; end--) {
      diff <<= 8;
      diff |= dbufd[end];
    }

    // negate delta if sign bit indicates it was negated during compression
    if ((code & 8) != 0) {
      diff = -diff;
    }

    // write out the uncompressed word
    fbufd[i] = prev + diff;
    __threadfence_block();

    // save prediction for next subchunk
    prev = fbufd[i + offset];
  }
}


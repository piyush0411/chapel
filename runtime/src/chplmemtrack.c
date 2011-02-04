#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "chplrt.h"
#include "chplmemtrack.h"
#include "chpltasks.h"
#include "chplcomm.h"
#include "error.h"

#undef malloc
#undef calloc
#undef free

typedef struct memTableEntry_struct { /* table entry */
  size_t number;
  size_t size;
  chpl_memDescInt_t description;
  void* memAlloc;
  int32_t lineno;
  chpl_string filename;
  struct memTableEntry_struct* nextInBucket;
} memTableEntry;

#define NUM_HASH_SIZE_INDICES 20

static int hashSizes[NUM_HASH_SIZE_INDICES] = { 1543, 3079, 6151, 12289, 24593, 49157, 98317,
                                                196613, 393241, 786433, 1572869, 3145739,
                                                6291469, 12582917, 25165843, 50331653,
                                                100663319, 201326611, 402653189, 805306457 };
static int hashSizeIndex = 0;
static int hashSize = 0;

static memTableEntry** memTable = NULL;

static _Bool memLeaks = false;
static _Bool memLeaksTable = false;
static _Bool memStats = false;
static uint64_t memMax = 0;
static _Bool memTrack = false;
static uint64_t memThreshold = 0;
static chpl_string memLog = NULL;
static FILE* memLogFile = NULL;
static chpl_string memLeaksLog = NULL;

static size_t totalMem = 0;       /* total memory currently allocated */
static size_t maxMem = 0;         /* maximum total memory during run  */
static size_t totalAllocated = 0; /* total memory allocated */
static size_t totalFreed = 0;     /* total memory freed */
static size_t totalEntries = 0;     /* number of entries in hash table */

static chpl_sync_aux_t memTrack_sync;

static unsigned hash(void* memAlloc, int hashSize) {
  unsigned hashValue = 0;
  char* fakeCharPtr = (char*)&memAlloc;
  size_t i;
  for (i = 0; i < sizeof(void*); i++) {
    hashValue = *fakeCharPtr + 31 * hashValue;
    fakeCharPtr++;
  }
  return hashValue % hashSize;
}


static void increaseMemStat(size_t chunk, int32_t lineno, chpl_string filename) {
  totalMem += chunk;
  totalAllocated += chunk;
  if (memMax && (totalMem > memMax)) {
    chpl_error("Exceeded memory limit", lineno, filename);
  }
  if (totalMem > maxMem)
    maxMem = totalMem;
}


static void decreaseMemStat(size_t chunk) {
  totalMem -= chunk; // > totalMem ? 0 : totalMem - chunk;
  totalFreed += chunk;
}


static void
resizeTable(int direction) {
  memTableEntry** newMemTable = NULL;
  int newHashSizeIndex, newHashSize, newHashValue;
  int i;
  memTableEntry* me;
  memTableEntry* next;

  newHashSizeIndex = hashSizeIndex + direction;
  newHashSize = hashSizes[newHashSizeIndex];
  newMemTable = calloc(newHashSize, sizeof(memTableEntry*));

  for (i = 0; i < hashSize; i++) {
    for (me = memTable[i]; me != NULL; me = next) {
      next = me->nextInBucket;
      newHashValue = hash(me->memAlloc, newHashSize);
      me->nextInBucket = newMemTable[newHashValue];
      newMemTable[newHashValue] = me;
    }
  }

  free(memTable);
  memTable = newMemTable;
  hashSize = newHashSize;
  hashSizeIndex = newHashSizeIndex;
}


static void addMemTableEntry(void* memAlloc, size_t number, size_t size, chpl_memDescInt_t description, int32_t lineno, chpl_string filename) {
  unsigned hashValue;
  memTableEntry* memEntry;

  if ((totalEntries+1)*2 > hashSize && hashSizeIndex < NUM_HASH_SIZE_INDICES-1)
    resizeTable(1);

  memEntry = (memTableEntry*) calloc(1, sizeof(memTableEntry));
  if (!memEntry) {
    chpl_error("memtrack fault: out of memory allocating memtrack table",
               lineno, filename);
  }

  hashValue = hash(memAlloc, hashSize);
  memEntry->nextInBucket = memTable[hashValue];
  memTable[hashValue] = memEntry;
  memEntry->description = description;
  memEntry->memAlloc = memAlloc;
  memEntry->lineno = lineno;
  memEntry->filename = filename;
  memEntry->number = number;
  memEntry->size = size;
  increaseMemStat(number*size, lineno, filename);
  totalEntries += 1;
}


static memTableEntry* removeMemTableEntry(void* address) {
  unsigned hashValue = hash(address, hashSize);
  memTableEntry* thisBucketEntry = memTable[hashValue];
  memTableEntry* deletedBucket = NULL;

  if (!thisBucketEntry)
    return NULL;

  if (thisBucketEntry->memAlloc == address) {
    memTable[hashValue] = thisBucketEntry->nextInBucket;
    deletedBucket = thisBucketEntry;
  } else {
    for (thisBucketEntry = memTable[hashValue];
         thisBucketEntry != NULL;
         thisBucketEntry = thisBucketEntry->nextInBucket) {

      memTableEntry* nextBucketEntry = thisBucketEntry->nextInBucket;

      if (nextBucketEntry && nextBucketEntry->memAlloc == address) {
        thisBucketEntry->nextInBucket = nextBucketEntry->nextInBucket;
        deletedBucket = nextBucketEntry;
      }
    }
  }
  if (deletedBucket) {
    decreaseMemStat(deletedBucket->number * deletedBucket->size);
    totalEntries -= 1;
    if (totalEntries*8 < hashSize && hashSizeIndex > 0)
      resizeTable(-1);
  }
  return deletedBucket;
}


void chpl_setMemFlags(chpl_bool memTrackConfig,
                      chpl_bool memStatsConfig,
                      chpl_bool memLeaksConfig,
                      chpl_bool memLeaksTableConfig,
                      uint64_t memMaxConfig,
                      uint64_t memThresholdConfig,
                      chpl_string memLogConfig,
                      chpl_string memLeaksLogConfig) {
  memTrack = memTrackConfig;
  if (memStatsConfig || memLeaksConfig || memLeaksTableConfig || memMaxConfig)
    memTrack = true;
  memStats = memStatsConfig;
  memLeaks = memLeaksConfig;
  memLeaksTable = memLeaksTableConfig;
  memMax = memMaxConfig;
  memThreshold = memThresholdConfig;
  memLog = memLogConfig;
  if (strcmp(memLog, "")) {
    if (chpl_numLocales > 1) {
      char* filename = (char*)malloc((strlen(memLog)+10)*sizeof(char));
      sprintf(filename, "%s.%"PRId32, memLog, chpl_localeID);
      memLogFile = fopen(filename, "w");
      free(filename);
    } else {
      memLogFile = fopen(memLog, "w");
    }
  } else {
    memLogFile = stdout;
  }
  memLeaksLog = memLeaksLogConfig;
  if (strcmp(memLeaksLog, ""))
    memTrack = true;

  if (memTrack) {
    chpl_sync_initAux(&memTrack_sync);
    hashSizeIndex = 0;
    hashSize = hashSizes[hashSizeIndex];
    memTable = calloc(hashSize, sizeof(memTableEntry*));
  }
}


uint64_t chpl_memoryUsed(int32_t lineno, chpl_string filename) {
  if (!memTrack)
    chpl_error("invalid call to memoryUsed(); rerun with --memTrack",
               lineno, filename);
  return (uint64_t)totalMem;
}


void chpl_printMemStat(int32_t lineno, chpl_string filename) {
  if (!memTrack)
    chpl_error("invalid call to printMemStat(); rerun with --memTrack",
               lineno, filename);
  chpl_sync_lock(&memTrack_sync);
  fprintf(memLogFile, "=================\n");
  fprintf(memLogFile, "Memory Statistics\n");
  if (chpl_numLocales == 1) {
    fprintf(memLogFile, "==============================================================\n");
    fprintf(memLogFile, "Current Allocated Memory               %zd\n", totalMem);
    fprintf(memLogFile, "Maximum Simultaneous Allocated Memory  %zd\n", maxMem);
    fprintf(memLogFile, "Total Allocated Memory                 %zd\n", totalAllocated);
    fprintf(memLogFile, "Total Freed Memory                     %zd\n", totalFreed);
    fprintf(memLogFile, "==============================================================\n");
  } else {
    int i;
    fprintf(memLogFile, "==============================================================\n");
    fprintf(memLogFile, "Locale\n");
    fprintf(memLogFile, "           Current Allocated Memory\n");
    fprintf(memLogFile, "                      Maximum Simultaneous Allocated Memory\n");
    fprintf(memLogFile, "                                 Total Allocated Memory\n");
    fprintf(memLogFile, "                                            Total Freed Memory\n");
    fprintf(memLogFile, "==============================================================\n");
    for (i = 0; i < chpl_numLocales; i++) {
      static size_t m1, m2, m3, m4;
      chpl_comm_get(&m1, i, &totalMem, sizeof(size_t), lineno, filename);
      chpl_comm_get(&m2, i, &maxMem, sizeof(size_t), lineno, filename);
      chpl_comm_get(&m3, i, &totalAllocated, sizeof(size_t), lineno, filename);
      chpl_comm_get(&m4, i, &totalFreed, sizeof(size_t), lineno, filename);
      fprintf(memLogFile, "%-9d  %-9zu  %-9zu  %-9zu  %-9zu\n", i, m1, m2, m3, m4);
    }
    fprintf(memLogFile, "==============================================================\n");
  }
  chpl_sync_unlock(&memTrack_sync);
}


static int leakedMemTableEntryCmp(const void* p1, const void* p2) {
  return *(size_t*)p2 - *(size_t*)p1;
}

static void chpl_printLeakedMemTable(void) {
  size_t* table;
  memTableEntry* me;
  int i;
  const int numberWidth   = 9;
  const int numEntries = CHPL_RT_MD_NUM+chpl_num_memDescs;

  table = (size_t*)calloc(numEntries, 3*sizeof(size_t));

  for (i = 0; i < hashSize; i++) {
    for (me = memTable[i]; me != NULL; me = me->nextInBucket) {
      table[3*me->description] += me->number*me->size;
      table[3*me->description+1] += 1;
      table[3*me->description+2] = me->description;
    }
  }

  qsort(table, numEntries, 3*sizeof(size_t), leakedMemTableEntryCmp);

  fprintf(memLogFile, "====================\n");
  fprintf(memLogFile, "Leaked Memory Report\n");
  fprintf(memLogFile, "==============================================================\n");
  fprintf(memLogFile, "Number of leaked allocations\n");
  fprintf(memLogFile, "           Total leaked memory (bytes)\n");
  fprintf(memLogFile, "                      Description of allocation\n");
  fprintf(memLogFile, "==============================================================\n");
  for (i = 0; i < 3*(CHPL_RT_MD_NUM+chpl_num_memDescs); i += 3) {
    if (table[i] > 0) {
      fprintf(memLogFile, "%-*zu  %-*zu  %s\n",
             numberWidth, table[i+1],
             numberWidth, table[i],
             chpl_memDescString(table[i+2]));
    }
  }
  fprintf(memLogFile, "==============================================================\n");

  free(table);
}


void chpl_reportMemInfo() {
  if (memStats) {
    fprintf(memLogFile, "\n");
    chpl_printMemStat(0, 0);
  }
  if (memLeaks) {
    fprintf(memLogFile, "\n");
    chpl_printLeakedMemTable();
  }
  if (memLeaksTable) {
    fprintf(memLogFile, "\n");
    chpl_printMemTable(0, 0, 0);
  }
  if (memLogFile && memLogFile != stdout)
    fclose(memLogFile);
  if (memLeaksLog && strcmp(memLeaksLog, "")) {
    memLogFile = fopen(memLeaksLog, "a");
    fprintf(memLogFile, "\nCompiler Command : %s\n", chpl_compileCommand);
    fprintf(memLogFile, "Execution Command: %s\n\n", chpl_executionCommand);
    chpl_printMemStat(0, 0);
    fprintf(memLogFile, "\n");
    chpl_printLeakedMemTable();
    fclose(memLogFile);
  }
}


static int descCmp(const void* p1, const void* p2) {
  memTableEntry* m1 = *(memTableEntry**)p1;
  memTableEntry* m2 = *(memTableEntry**)p2;

  int val = strcmp(chpl_memDescString(m1->description), chpl_memDescString(m2->description));
  if (val == 0 && m1->filename && m2->filename)
    val = strcmp(m1->filename, m2->filename);
  if (val == 0)
    val = (m1->lineno < m2->lineno) ? -1 : ((m1->lineno > m2->lineno) ? 1 : 0);
  return val;
}


void chpl_printMemTable(int64_t threshold, int32_t lineno, chpl_string filename) {
  const int numberWidth   = 9;
  const int precision     = sizeof(uintptr_t) * 2;
  const int addressWidth  = precision+4;
  const int descWidth     = 33;
  int filenameWidth       = strlen("Allocated Memory (Bytes)");
  int totalWidth;

  memTableEntry* memEntry;
  int n, i;
  char* loc;
  memTableEntry** table;

  if (!memTrack)
    chpl_error("The printMemTable function only works with the --memTrack flag", lineno, filename);

  n = 0;
  filenameWidth = strlen("Allocated Memory (Bytes)");
  for (i = 0; i < hashSize; i++) {
    for (memEntry = memTable[i]; memEntry != NULL; memEntry = memEntry->nextInBucket) {
      size_t chunk = memEntry->number * memEntry->size;
      if (chunk >= threshold) {
        n += 1;
        if (memEntry->filename) {
          int filenameLength = strlen(memEntry->filename);
          if (filenameLength > filenameWidth)
            filenameWidth = filenameLength;
        }
      }
    }
  }

  totalWidth = filenameWidth+numberWidth*4+descWidth+20;
  for (i = 0; i < totalWidth; i++)
    fprintf(memLogFile, "=");
  fprintf(memLogFile, "\n");
  fprintf(memLogFile, "%-*s%-*s%-*s%-*s%-*s%-*s\n",
         filenameWidth+numberWidth, "Allocated Memory (Bytes)",
         numberWidth, "Number",
         numberWidth, "Size",
         numberWidth, "Total",
         descWidth, "Description",
         20, "Address");
  for (i = 0; i < totalWidth; i++)
    fprintf(memLogFile, "=");
  fprintf(memLogFile, "\n");

  table = (memTableEntry**)malloc(n*sizeof(memTableEntry*));
  if (!table)
    chpl_error("out of memory printing memory table", lineno, filename);

  n = 0;
  for (i = 0; i < hashSize; i++) {
    for (memEntry = memTable[i]; memEntry != NULL; memEntry = memEntry->nextInBucket) {
      size_t chunk = memEntry->number * memEntry->size;
      if (chunk >= threshold) {
        table[n++] = memEntry;
      }
    }
  }
  qsort(table, n, sizeof(memTableEntry*), descCmp);

  loc = (char*)malloc((filenameWidth+numberWidth+1)*sizeof(char));

  for (i = 0; i < n; i++) {
    memEntry = table[i];
    if (memEntry->filename)
      sprintf(loc, "%s:%"PRId32, memEntry->filename, memEntry->lineno);
    else
      sprintf(loc, "--");
    fprintf(memLogFile, "%-*s%-*zu%-*zu%-*zu%-*s%#-*.*" PRIxPTR "\n",
           filenameWidth+numberWidth, loc,
           numberWidth, memEntry->number,
           numberWidth, memEntry->size,
           numberWidth, memEntry->size*memEntry->number,
           descWidth, chpl_memDescString(memEntry->description),
           addressWidth, precision, (uintptr_t)memEntry->memAlloc);
  }
  for (i = 0; i < totalWidth; i++)
    fprintf(memLogFile, "=");
  fprintf(memLogFile, "\n");
  putchar('\n');

  free(table);
  free(loc);
}


void chpl_track_malloc(void* memAlloc, size_t chunk, size_t number, size_t size, chpl_memDescInt_t description, int32_t lineno, chpl_string filename) {
  if (chunk > memThreshold) {
    if (memTrack) {
      chpl_sync_lock(&memTrack_sync);
      addMemTableEntry(memAlloc, number, size, description, lineno, filename);
      chpl_sync_unlock(&memTrack_sync);
    }
    if (chpl_verbose_mem)
      fprintf(memLogFile, "%"PRId32": %s:%"PRId32": allocate %zuB of %s at %p\n", chpl_localeID, (filename ? filename : "--"), lineno, number*size, chpl_memDescString(description), memAlloc);
  }
}


void chpl_track_free(void* memAlloc, int32_t lineno, chpl_string filename) {
  memTableEntry* memEntry = NULL;

  if (memTrack) {
    chpl_sync_lock(&memTrack_sync);
    memEntry = removeMemTableEntry(memAlloc);
    if (memEntry) {
      if (chpl_verbose_mem)
        fprintf(memLogFile, "%"PRId32": %s:%"PRId32": free %zuB of %s at %p\n", chpl_localeID, (filename ? filename : "--"), lineno, memEntry->number*memEntry->size, chpl_memDescString(memEntry->description), memAlloc);
      free(memEntry);
    }
    chpl_sync_unlock(&memTrack_sync);
  } else if (chpl_verbose_mem && !memEntry) {
    fprintf(memLogFile, "%"PRId32": %s:%"PRId32": free at %p\n", chpl_localeID, (filename ? filename : "--"), lineno, memAlloc);
  }
}


void chpl_track_realloc1(void* memAlloc, size_t number, size_t size, chpl_memDescInt_t description, int32_t lineno, chpl_string filename) {
  memTableEntry* memEntry = NULL;

  if (memTrack && number*size > memThreshold) {
    chpl_sync_lock(&memTrack_sync);
    if (memAlloc) {
      memEntry = removeMemTableEntry(memAlloc);
      if (memEntry)
        free(memEntry);
    }
    chpl_sync_unlock(&memTrack_sync);
  }
}


void chpl_track_realloc2(void* moreMemAlloc, size_t newChunk, void* memAlloc, size_t number, size_t size, chpl_memDescInt_t description, int32_t lineno, chpl_string filename) {
  if (newChunk > memThreshold) {
    if (memTrack) {
      chpl_sync_lock(&memTrack_sync);
      addMemTableEntry(moreMemAlloc, number, size, description, lineno, filename);
      chpl_sync_unlock(&memTrack_sync);
    }
    if (chpl_verbose_mem)
      fprintf(memLogFile, "%"PRId32": %s:%"PRId32": reallocate %zuB of %s at %p -> %p\n", chpl_localeID, (filename ? filename : "--"), lineno, number*size, chpl_memDescString(description), memAlloc, moreMemAlloc);
  }
}

void chpl_startVerboseMem() {
  chpl_verbose_mem = 1;
  chpl_comm_broadcast_private(2 /* &chpl_verbose_mem */, sizeof(int));
}

void chpl_stopVerboseMem() {
  chpl_verbose_mem = 0;
  chpl_comm_broadcast_private(2 /* &chpl_verbose_mem */, sizeof(int));
}

void chpl_startVerboseMemHere() {
  chpl_verbose_mem = 1;
}

void chpl_stopVerboseMemHere() {
  chpl_verbose_mem = 0;
}

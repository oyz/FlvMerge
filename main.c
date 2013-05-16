#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if (defined(__WIN32__) || defined(_WIN32))
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include <assert.h>

typedef int Boolean;
#define True 1
#define False 0

#ifndef SInt64
#define SInt64 long long
#endif
#ifndef UInt32
#define UInt32 unsigned int
#endif
#ifndef UInt8
#define UInt8 unsigned char
#endif

#if (defined(__WIN32__) || defined(_WIN32)) && !defined(IMN_PIM)
// For Windoze, we need to implement our own gettimeofday()
#if !defined(_WIN32_WCE)
#include <sys/timeb.h>
#endif

static int gettimeofday(struct timeval* tp, int* /*tz*/) {
    static LARGE_INTEGER tickFrequency, epochOffset;

    // For our first call, use "ftime()", so that we get a time with a proper epoch.
    // For subsequent calls, use "QueryPerformanceCount()", because it's more fine-grain.
    static Boolean isFirstCall = True;

    LARGE_INTEGER tickNow;
    QueryPerformanceCounter(&tickNow);

    if (isFirstCall) {
	struct timeb tb;
	ftime(&tb);
	tp->tv_sec = tb.time;
	tp->tv_usec = 1000*tb.millitm;

	// Also get our counter frequency:
	QueryPerformanceFrequency(&tickFrequency);

	// And compute an offset to add to subsequent counter times, so we get a proper epoch:
	epochOffset.QuadPart
	  = tb.time*tickFrequency.QuadPart + (tb.millitm*tickFrequency.QuadPart)/1000 - tickNow.QuadPart;

	isFirstCall = False; // for next time
    } else {
	// Adjust our counter time so that we get a proper epoch:
	tickNow.QuadPart += epochOffset.QuadPart;

	tp->tv_sec = (long) (tickNow.QuadPart / tickFrequency.QuadPart);
	tp->tv_usec = (long) (((tickNow.QuadPart % tickFrequency.QuadPart) * 1000000L) / tickFrequency.QuadPart);
    }

    return 0;
}
#endif

static SInt64 GetCurrentTimeInMicroseconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (SInt64)tv.tv_sec*1000000 + tv.tv_usec;
}

#define FLV_HEADER_SIZE 9
#define FLV_TAG_HEADER_SIZE 11

typedef struct FLVContext
{
    unsigned char     soundFormat;
    unsigned char     soundRate;
    unsigned char     soundSize;
    unsigned char     soundType;
	
    unsigned char     videoCodecID;
	
    FILE              *fileSource;
    struct FLVContext *next;
}FLVContext;

typedef struct MergeTaskList
{
    FLVContext *first;
    FLVContext *last;
    UInt32     count;

    void (*add_task)(struct MergeTaskList *taskList, FLVContext *flvCtx);
    int  (*do_merge_tasks)(struct MergeTaskList *taskList, const char* const mergeFileName);
    void (*clear_tasks)(struct MergeTaskList *taskList);
}MergeTaskList;

static void PrintUsagesAndExit(void)
{
    fprintf(stderr, "usages:- [inputfilename1] [inputfilename2] ... [inputfilenameN] [mergefilename]\n"
	    "(must have two input files at least)\n");
    exit(1);
}

static int InitMergeTaskList(MergeTaskList *taskList);
static int InitFLVContext(FLVContext **flvCtx, const char* const fileName);
static int IsSuitableToMerge(FLVContext *flvCtx1, FLVContext *flvCtx2);
static int IsFLVFile(FILE *input);
static UInt32 FromInt32StringBe(const UInt8* const str);
static UInt32 FromInt24StringBe(const UInt8* const str);
static int GetFLVFileInfo(FLVContext *flvCtx);
static int AddFileData(FILE* inputFile, FILE* mergeFile, int isFirstFile, UInt32* lastTimestamp);
static int ReadFromFile(FILE* inputFile, char* const buffer, int size);
static int WriteToFile(FILE* outputFile, char* const buffer, int size);
static UInt32 GetTimestamp(const UInt8* const pos);
static void SetTimestamp(UInt8* const pos, UInt32 newTimestamp);
static void DeleteFLVContext(FLVContext *flvCtx);
static void AddToMergeTaskList(MergeTaskList *taskList, FLVContext *flvCtx);
static int DoMergeTasks(MergeTaskList *taskList, const char* const mergeFileName);
static void ClearTasks(struct MergeTaskList *taskList);

UInt32 FromInt32StringBe(const UInt8* const str)
{
    return ((str[0]<<24)  |
	    (str[1]<<16)   |
	    (str[2]<<8)    |
	    (str[3]));
}

UInt32 FromInt24StringBe(const UInt8* const str)
{
    return ((str[0]<<16)  |
	    (str[1]<<8)   |
	    (str[2]));
}

UInt32 GetTimestamp(const UInt8* const pos)
{
    return ((pos[3]<<24)   |
            (pos[0]<<16)   |
	    (pos[1]<<8)    |
	    (pos[2]));
}

int ReadFromFile(FILE* inputFile, char* const buffer, int size)
{
    int readLen, realReadLen;
    char* tmp;

    assert(inputFile != NULL && buffer != NULL);

    if (inputFile != NULL && buffer != NULL) {
	readLen = size;
	realReadLen = 0;
	tmp = buffer;
	while ( (realReadLen=fread(tmp, sizeof(char), readLen, inputFile)) > 0) {
	    readLen -= realReadLen;
	    tmp += realReadLen;
	}

	return (readLen==0)?size:-1;
    }

    return -1;	
}

int WriteToFile(FILE* outputFile, char* const buffer, int size)
{
    int writeLen, realWriteLen;
    char* tmp;

    assert(outputFile != NULL && buffer != NULL);

    if (outputFile != NULL && buffer != NULL) {
	writeLen = size;
	realWriteLen = 0;
	tmp = buffer;
	while ( (realWriteLen=fwrite(tmp, sizeof(char), writeLen, outputFile)) > 0) {
	    writeLen -= realWriteLen;
	    tmp += realWriteLen;
	}

	return (writeLen==0)?size:-1;
    }

    return -1;
}

void SetTimestamp(UInt8* const pos, UInt32 newTimestamp)
{
    pos[3] = newTimestamp>>24;
    pos[0] = newTimestamp>>16;
    pos[1] = newTimestamp>>8;
    pos[2] = newTimestamp;
}

int InitFLVContext(FLVContext **flvCtx, const char* const fileName)
{
    assert(flvCtx != NULL);

    if (flvCtx == NULL) return -1;

    if ( NULL == (*flvCtx=(FLVContext*)calloc(1, sizeof(FLVContext))) ) {
	fprintf(stderr, "malloc FLVContext error!\n");
	return -1;
    }

    if ( NULL == ((*flvCtx)->fileSource=fopen(fileName, "rb")) ) {
	fprintf(stderr, "open file %s failed.\n", fileName);
	goto failed;
    }

    if (!IsFLVFile((*flvCtx)->fileSource)) {
	fprintf(stderr, "%s: invalid FLV file!", fileName);
	goto failed;
    }

    if (GetFLVFileInfo(*flvCtx) != 0) {
	fprintf(stderr, "cannot find flv file info!\n");
	goto failed;
    }

    return 0;

 failed:
    if ((*flvCtx)->fileSource != NULL)
      fclose((*flvCtx)->fileSource);
	
    free(*flvCtx);
    *flvCtx = NULL;
	
    return -1;
}

void DeleteFLVContext(FLVContext *flvCtx)
{
    assert(flvCtx != NULL);

    if ( flvCtx != NULL ) {
	if ( flvCtx->fileSource != NULL )
	  fclose(flvCtx->fileSource);
	free(flvCtx);
    }
}

int InitMergeTaskList(MergeTaskList *taskList)
{
    assert(taskList != NULL);

    if (taskList == NULL) return -1;

    memset(taskList, 0, sizeof(MergeTaskList));
	
    // set callbacks
    taskList->add_task = AddToMergeTaskList;
    taskList->do_merge_tasks = DoMergeTasks;
    taskList->clear_tasks = ClearTasks;

    return 0;
}

int IsFLVFile(FILE *input)
{
    int len;
    unsigned char buf[FLV_HEADER_SIZE];
    assert(input != NULL);

    if (input == NULL) return 0;

    rewind(input);

    if ( FLV_HEADER_SIZE != (len=fread(buf, sizeof(unsigned char), FLV_HEADER_SIZE, input)) ) {
	return 0;
    }

    if (buf[0] != 'F' || buf[1] != 'L' || buf[2] != 'V' 
	|| buf[3] != 0x01 || FromInt32StringBe(&buf[5]) != 9)
      return 0;

    return 1;
}

int GetFLVFileInfo(FLVContext *flvCtx)
{
    int hasAudioParams, hasVideoParams;
    int skipSize, readLen;
    int dataSize;
    UInt8 tagType;
    UInt8 tmp[FLV_TAG_HEADER_SIZE+1];

    assert(flvCtx != NULL);

    if (flvCtx == NULL) return -1;

    if (flvCtx->fileSource != NULL) {
	rewind(flvCtx->fileSource);

	skipSize = 9;
	if (fseek(flvCtx->fileSource, skipSize, SEEK_CUR) != 0)
	  return -1;
	hasVideoParams = hasAudioParams = 0;
	skipSize = 4;
	while (!hasVideoParams || !hasAudioParams) {
	    if (fseek(flvCtx->fileSource, skipSize, SEEK_CUR) != 0)
	      return -1;

	    if (FLV_TAG_HEADER_SIZE+1 != (readLen = fread(tmp, sizeof(UInt8), FLV_TAG_HEADER_SIZE+1, flvCtx->fileSource)))
	      return -1;

	    tagType = tmp[0] & 0x1f;

	    switch (tagType) {
	      case 8 :
		flvCtx->soundFormat = (tmp[FLV_TAG_HEADER_SIZE] & 0xf0) >> 4 ;
		flvCtx->soundRate   = (tmp[FLV_TAG_HEADER_SIZE] & 0x0c) >> 2 ;
		flvCtx->soundSize   = (tmp[FLV_TAG_HEADER_SIZE] & 0x02) >> 1 ;
		flvCtx->soundType   = (tmp[FLV_TAG_HEADER_SIZE] & 0x01) >> 0 ;
		hasAudioParams = 1;
		break;
	      case 9 :
		flvCtx->videoCodecID = (tmp[FLV_TAG_HEADER_SIZE] & 0x0f);
		hasVideoParams = 1;
		break;
	      default :
		break;
	    }

	    dataSize = FromInt24StringBe(&tmp[1]);

	    skipSize = dataSize - 1 + 4;
	}

	return 0;
    } else {
	return -1;
    }
}

int IsSuitableToMerge(FLVContext *flvCtx1, FLVContext *flvCtx2)
{
    return (flvCtx1->soundFormat == flvCtx2->soundFormat) &&
      (flvCtx1->soundRate == flvCtx2->soundRate) &&
      (flvCtx1->soundSize == flvCtx2->soundSize) &&
      (flvCtx1->soundType == flvCtx2->soundType) &&
      (flvCtx1->videoCodecID == flvCtx2->videoCodecID);
}

#define MAX_DATA_SIZE 16777220

int AddFileData(FILE* inputFile, FILE* mergeFile, int isFirstFile, UInt32* lastTimestamp)
{
    int readLen;
    UInt32 curTimestamp = 0;
    UInt32 newTimestamp = 0;
    int dataSize;
    UInt8 tmp[20];
    char *buf;

    assert(inputFile != NULL && mergeFile != NULL);

    if (NULL == (buf=(char*)malloc(sizeof(char)*MAX_DATA_SIZE))) {
	fprintf(stderr, "malloc error!\n");
	return -1;
    }

    if (inputFile != NULL && mergeFile != NULL){
	rewind(inputFile);

	if (isFirstFile) {
	    if ( FLV_HEADER_SIZE+4 == (readLen=fread(tmp, sizeof(UInt8), FLV_HEADER_SIZE+4, inputFile)) ) {
		rewind(mergeFile);
		if (readLen != fwrite(tmp, sizeof(char), 
				      readLen, mergeFile)) {
		    goto failed;
		}
	    } else {
		goto failed;
	    }
	} else {
	    if (fseek(inputFile, FLV_HEADER_SIZE+4, SEEK_CUR) != 0)
	      goto failed;
	}

	while (ReadFromFile(inputFile, (char*)tmp, FLV_TAG_HEADER_SIZE) > 0) {
	    dataSize = FromInt24StringBe(&tmp[1]);

	    curTimestamp = GetTimestamp(&tmp[4]);
	    newTimestamp = curTimestamp + *lastTimestamp;
	    SetTimestamp(&tmp[4], newTimestamp);

	    if (WriteToFile(mergeFile, (char*)tmp, FLV_TAG_HEADER_SIZE) < 0)
	      goto failed;

	    readLen = dataSize+4;
			
	    if (ReadFromFile(inputFile, buf, readLen) > 0) {
		if (WriteToFile(mergeFile, buf, readLen) < 0)
		  goto failed;
	    } else {
		goto failed;
	    }
	}

	// update the timestamp and return
	*lastTimestamp = newTimestamp;

	free(buf);
	buf = NULL;
	return 0;
    }

 failed:
    free(buf);
    buf = NULL;
    return -1;
}

void AddToMergeTaskList(MergeTaskList *taskList, FLVContext *flvCtx)
{
    assert(taskList != NULL && flvCtx != NULL);

    if (taskList != NULL && flvCtx != NULL) {
	if (taskList->count == 0) {
	    taskList->first = taskList->last = flvCtx;
	} else {
	    taskList->last->next = flvCtx;
	    taskList->last = taskList->last->next;
	}
	taskList->count++;
    }
}


int DoMergeTasks(MergeTaskList *taskList, const char* const mergeFileName)
{
    assert(taskList != NULL && mergeFileName != NULL);

    int i;
    FILE *mergeFile;
    UInt32 lastTimestamp = 0;
    FLVContext *curCtx, *nextCtx;

    if (taskList == NULL) return -1;

    if ( NULL == (mergeFile=fopen(mergeFileName, "wb")) ) {
	fprintf(stderr, "open file %s failed!\n", mergeFileName);
	return -1;
    }

    // insure the flv files are suitable to merge
    curCtx = taskList->first;
    for (i = 0; i < taskList->count-1; i++) {
	nextCtx = curCtx->next;

	if (!IsSuitableToMerge(curCtx, nextCtx)) {
	    fprintf(stderr, "unable to merge the flv files, maybe different parameters!\n");
	    goto failed;
	}

	curCtx = nextCtx;
    }

    // combine them one by one
    curCtx = taskList->first;
    lastTimestamp = 0;
    for (i = 0; i < taskList->count; i++) {
	if ( AddFileData(curCtx->fileSource, mergeFile, (i==0), &lastTimestamp) != 0 ) {
	    goto failed;
	}
	curCtx = curCtx->next;
    }

    return 0;

 failed:
    fclose(mergeFile);
    return -1;
}

void ClearTasks(struct MergeTaskList *taskList)
{
    assert(taskList != NULL);

    FLVContext *curCtx, *nextCtx;

    if (taskList != NULL) {

	curCtx = taskList->first;
	while (taskList->count--) {
	    nextCtx = curCtx->next;

	    DeleteFLVContext(curCtx);
			
	    curCtx = nextCtx;
	}
    }
}

int main(int argc, char* argv[])
{
    int i;
    char *inputFileName, *mergeFileName;
    FLVContext *flvCtx;
    MergeTaskList mergeTaskList;
    SInt64 ti = GetCurrentTimeInMicroseconds();

    if (InitMergeTaskList(&mergeTaskList) != 0)
      exit(1);

    if (argc < 4) {
	PrintUsagesAndExit();
    } else {
	for (i = 1; i < argc-1; i++) {
	    inputFileName = argv[i];
	    if (InitFLVContext(&flvCtx, inputFileName) != 0) {
		fprintf(stderr, "failed when init file %s!\n", inputFileName);
		goto failed;
	    } else {
		mergeTaskList.add_task(&mergeTaskList, flvCtx);
	    }
	}

	mergeFileName = argv[argc-1];
		
	if (mergeTaskList.do_merge_tasks(&mergeTaskList, mergeFileName) == 0) {
	    fprintf(stderr, "-----congratulations, merge success!------\n");
	    ti = GetCurrentTimeInMicroseconds() - ti;
	    fprintf(stderr, "bench: used %0.3fs \n", ti/1000000.0);
	} else {
	    fprintf(stderr, "failed!!!\n");
	}
    }

    exit(0);

 failed:
    mergeTaskList.clear_tasks(&mergeTaskList);
    exit(-1);
}

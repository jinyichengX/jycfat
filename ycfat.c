#include "ycfat_config.h"
#include "mheap.h"
#include "list.h"

typedef unsigned char   J_UINT8;
typedef unsigned short  J_UINT16;
typedef unsigned int    J_UINT32;
typedef unsigned int    J_UINT64;

typedef enum
{
    NOTFOUND = -1,
    FOUND = 0,
}SeekFile; 

#define USE_FAT32 1
#if USE_FAT32
#define FAT_SIZE 4
#define FATSIZE(S_DISK,S_CLUS) FAT_SIZE*S_DISK/S_CLUS /* FAT1/FAT2 表大小(字节) */
#endif

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

static unsigned int emp_clu = 0;

/* 定义首目录簇开始扇区 */
static unsigned int FirstDirSector = 0;

/* 定义FAT32扇区大小，固定为512Byte */
#define PER_SECSIZE 512

/* 定义根目录簇 */
#define ROOT_CLUS   2

/* FAT32中文件目录项DFT（文件属性）中起始簇偏移+2 */
#define START_SECTOR_OF_FILE(clu) (((clu-2)*32)+FirstDirSector)

/* 检查文件信息中的文件属性字段 */
#define CHECK_FDI_ATTR(x) x->attribute

/* 是否为文件末尾 */
#define IS_EOF(clu) (clu == 0x0fffffff)

#define ARGVS_ERROR -99

/* 文件名不允许的字符：
反斜杠 ()：在FAT32中，反斜杠用作目录分隔符，因此不能在文件名中使用。
正斜杠 (/)：与反斜杠一样，正斜杠也被用作目录分隔符，不能在文件名中使用。
冒号 (:)：冒号在FAT32中有特殊含义，用于分隔驱动器名和路径，因此不能在文件名中使用。
星号 (*)：星号在FAT32中被视为通配符，不能用于文件名。
问号 (?)：问号也被视为通配符，在文件名中不允许使用。
双引号 (")：双引号也不允许在文件名中使用。
尖括号 (< >)：尖括号也被禁止在文件名中。
竖线 (|)：竖线也不能在文件名中使用。
分号 (;)：分号也不允许在文件名中使用。
逗号 (,)：逗号也被禁止在文件名中。*/
/* 对于其他特殊字符，某些操作系统或应用程序可能对这些字符有特殊处理，请谨慎使用 */
/* 文件名是否合规 */
#define IS_FILENAME_ILLEGAL(fn) \
({ \
    J_UINT8 isIllegal = 1; \
    while (*fn) \
    { \
        if ((*fn == '\\') || (*fn == '/') || (*fn == ':') || (*fn == '*') || \
            (*fn == '?') || (*fn == '"') || (*fn == '<') || (*fn == '>') || \
            (*fn == '|') || (*fn == ';') || (*fn == ',') || (*fn == ' ')) \
        { \
            isIllegal = 0; \
            break; \
        } \
        fn++; \
    } \
    isIllegal; \
})

/* 初始化参数 */
struct FatInitArgs {
    unsigned int DBR_Ss;          /* DBR起始扇区 */
    unsigned int FirstDirSector;  /* 根目录起始簇 */
    unsigned int FAT1Sec;         /* FAT1表起始扇区 */
   
    unsigned int FreeClusNum;     /* 剩余空簇数目 */
    unsigned int NextFreeClu;     /* 下一个空簇 */
};
struct FatInitArgs FatInitArgs_a[4];

/* 由簇号锚定其所在FAT表扇区 */
#define CLU_TO_FATSEC(clu) ((clu * FAT_SIZE / PER_SECSIZE) + FatInitArgs_a[0].FAT1Sec)

/* 定义分区属性，16Byte */
typedef struct DiskPartitionTable
{
    J_UINT8 isActive;           /* 引导指示符 */
    J_UINT8 partStartHead;      /* 分区开始磁头 */
    J_UINT16 startCylSec;       /* 开始柱面与扇区 */
    J_UINT8 partType;           /* 分区类型，定义分区类型
                                    本系统的所有分区类型为FAT32 */
    J_UINT8 partEndHead;        /* 分区结束磁头 */
    J_UINT16 endCylSect;        /* 结束柱面和扇区 */
    J_UINT32 partStartSec;      /* 分区开始扇区 */
    J_UINT32 perPartSecNum;     /* 分区所占总扇区数 */
}DPT_t,*DPT;

/* 定义主引导头，占用FAT32的第一个扇区 */
typedef struct MasterBootRecord
{
    J_UINT8 BootLoader[446];    /* 引导程序 */
    DPT_t dpt[4];               /* 分区表，FAT32至多支持4个分区
                                    即一个物理磁盘至多可以划分成四个逻辑磁盘 */
    J_UINT8 fixEndMark[2];      /* 固定标记码0x55,0xAA */
}MBR_t,*MBR;

/* BIOS PARAMETER BLOCK DATA STRUCTURE */
typedef struct DOSBootRecord{
  J_UINT8 jmpBoot[3];   /* 跳转指令 */
  J_UINT8 OEMName[8];   /* OEM厂商 */

  /* BOIS param block */
  J_UINT16 bytsPerSec;  /* 每扇区大小，通常为512 */
  J_UINT8 secPerClus;   /* 每簇扇区数 */
  J_UINT16 rsvdSecCnt;  /* 保留扇区数（DBR->FAT1） */
  J_UINT8 numFATs;      /* FAT表数，通常为2 */

  J_UINT16 rootEntCnt;  /* 根目录项数，FAT32为0 */
  J_UINT16 totSec16;    /* 小扇区数，FAT32为0 */
  J_UINT8 media;        /* 媒体描述符，主要用于FAT16 */
  J_UINT16 FATSz16;     /* FAT扇区数（针对FAT12/16） */
  J_UINT16 secPerTrk;   /* 每道扇区数 */
  J_UINT16 numHeads;    /* 磁头数 */
  J_UINT32 hiddSec;     /* 隐藏扇区数 */

  J_UINT32 totSec32;    /* 总扇区数 */
  J_UINT32 FATSz32;     /* 每个FAT的扇区数，FAT32专用 */

  J_UINT16 extFlags; /* 扩展标志 */
  J_UINT16 FSVer; /* 文件系统版本 */
  J_UINT32 rootClusNum; /* 根目录簇号 */
  J_UINT16 FSInfo; /* 通常为1 */
  J_UINT16 bkBootSec; /* 通常为6 */
  J_UINT8 reserved[12]; /* 总为0 */
  J_UINT8 drvNum; /* 物理驱动器号 */
  J_UINT8 reserved1;  /* 总为0 */
  J_UINT8 exBootSig; /* 扩展引导标签 */
  J_UINT32 vollD; /* 分区序号 */
  J_UINT8 volLab[11]; /* 卷标 */
  J_UINT8 filSysType[8]; /* 系统ID */
}DBR_t;

typedef struct FileSystemInfoStruct
{
    J_UINT8 Head[4];//"RRaA"
    J_UINT8 Resv1[480];
    J_UINT8 Sign[4]; //"rrAa"
    J_UINT8 Free_nClus[4]; //剩余空簇数量
    J_UINT8 Next_Free_Clus[4];//下一个剩余空簇
    J_UINT8 Resv2[14];
    J_UINT8 FixTail[2];//"55 AA"
}FSINFO_t;

typedef enum
{
    RD_WR = 0X00, /* 读写 */
    RD_ONLY = 0X01, /* 只读 */
    HIDDEN = 0X02,/* 隐藏 */
    SYSTEM = 0X40,/* 系统 */
    VOLUME = 0X08, /* 卷标 */
    TP_DIR = 0X10,/* 子目录 */
    ARCHIVE = 0X20, /* 存档 */
}FLA;

/* 文件信息 */
typedef struct FileDirectoryItem
{
    J_UINT8 fileName[8];    /* 文件名 */
    J_UINT8 extName[3];        /* 扩展文件名 */

    union{
        J_UINT8 attribute;    /* 属性 */
        FLA attribute1;         /* 属性 */
    };
    
    J_UINT8 UpLower;        /* 文件名大小写标志位 */
    J_UINT8 crtTime_10ms; /* 创建时间10ms */
    union{
        J_UINT16 crtTime;J_UINT8 crtTime_a[2];       /* 创建时间 */
    };
    union{
        J_UINT16 crtDate;J_UINT8 crtDate_a[2];       /* 创建日期 */
    };
    union{
        J_UINT16 acsDate;J_UINT8 acsDate_a[2];       /* 最后访问日期 */
    };
    J_UINT8 startClusUper[2];      /* 起始簇高16位 */
    J_UINT8 modTime[2];         /* 最近修改时间 */
    J_UINT8 modDate[2];         /* 最近修改日期 */
    J_UINT8 startClusLower[2];  /* 起始簇低16位 */
    J_UINT8 fileSize[4];        /* 文件大小（字节） */
}FDI_t;

/* 一个扇区内的FDI */
typedef struct FDIInOneSec
{
    FDI_t fdi[PER_SECSIZE/sizeof(FDI_t)];
}FDIs_t;

/* 全局MBR DBR */
MBR_t g_mbr;DBR_t g_dbr[4];
static unsigned char g_dbr_n = 0;

typedef enum{
    FILE_CLOSE,
    FILE_OPEN
}FILE_STATE;

/* 文件句柄 */
typedef struct fileHandler
{
     /* 文件描述符 */
    unsigned int fd;
    unsigned int FirstClu;
    /* 数据锚定（读） */
    unsigned int CurClus;   /* 当前簇 */
    short CurOffSec;    /* 当前簇内偏移扇区 */
    unsigned short CurOffByte;  /* 当前扇区内偏移字节 */
    /* 文件大小 */
    unsigned int fl_sz;
    /* 剩余大小 */
    unsigned int left_sz;
    /* 文件状态 */
    FILE_STATE file_state;
#if YC_FILE2MEM
    int (*load2memory)(FILE *,void *mem_base,int);/* 文件是否加载至内存操作 */
    int (*Writeback)(FILE *,void *mem_base,int);/* 文件回写 */
#endif
    struct list_head WRCluChainList;/* 簇链缓冲头节点，不携带实际数据 */
    /* 文件末簇 */
    unsigned int EndClu;
    /* 文件末簇未写大小 */
    unsigned int EndCluLeftSize;
}FILE;

typedef struct WRCluChainBuffer
{
    struct list_head WRCluChainNode;
    unsigned int w_s_clu;    /* 头簇 */
    unsigned int w_e_clu;    /* 尾簇 */
}w_buffer_t; 

// bit map for FAT table
/* 只定义一个位图，不支持多磁盘分区 */
/* FAT进行位图映射时，直接将FAT值和0做逻辑或运算 */
uint8_t clusterBitmap[(PER_SECSIZE/FAT_SIZE)/8];//16Bytes
static unsigned int cur_fat_sec;

/* 当前所在目录 */
char pwd[4][50] = {0};
pwd[0][0] = pwd[1][0] = pwd[2][0] = pwd[3][0] = '/'; 
static unsigned int work_clu[4] = {2,2,2,2};

void usr_read(void * buffer,unsigned int SecIndex,unsigned int SecNum){}
void usr_write(void * buffer,unsigned int SecIndex,unsigned int SecNum){}
/* 将Byte转化为数值 */
unsigned int Byte2Value(unsigned char *data,unsigned char len)
{
    if(len > 4) return 0;

    unsigned int temp = 0;
    if(len >= 1) temp|=((unsigned char)data[0]);
    if(len >= 2) temp|=((unsigned char)data[1])<<8;
    if(len >= 3) temp|=((unsigned char)data[2])<<16;
    if(len >= 4) temp|=((unsigned char)data[3])<<24;

    return temp;
}

void YC_FAT_ReadDBR(DBR_t * dbr_n);

/* 解析绝对0扇区的MBR或DBR */
void YC_FAT_AnalyseSec0(void)
{
    MBR_t * mbr = (MBR_t *)&g_mbr;

    unsigned char buffer[PER_SECSIZE];

    /* 读取绝对0扇区 */
    usr_read(&buffer,0,PER_SECSIZE);

    /* 判断绝对0扇区是不是为MBR扇区 */
    if((*buffer == 0xEB)&&(*(buffer+1) == 0x58)&&(*(buffer+2) == 0x90))
    {
        g_dbr_n = 0;
    }

    /* 解析分区开始扇区和分区所占总扇区数 */
    for(unsigned char i = 0;i < 4 ; i++)
    {
        if( 0 == (unsigned char *)(buffer+446+16*i+8) )
            continue;
        mbr->dpt[i].partStartSec = Byte2Value((unsigned char *)(buffer+446+16*i+8),4);
        g_dbr_n ++;
    }

    /* DBR初始化 */
    if(!g_dbr_n)
    {
        YC_FAT_ReadDBR((DBR_t *)&g_dbr[0]);
        /* 初始化系统参数 */
        FatInitArgs_a[0].FAT1Sec = g_dbr[0].rsvdSecCnt; /* FAT1起始扇区等于保留扇区数 */
        FatInitArgs_a[0].FirstDirSector = FatInitArgs_a[0].FAT1Sec\
                            + (g_dbr[0].numFATs * g_dbr[0].FATSz32);
    }else{
        for(unsigned char i = 0;i<g_dbr_n;i++)
        {
            YC_FAT_ReadDBR((DBR_t *)&g_dbr[i]);
            /* 初始化系统参数 */
            FatInitArgs_a[i].FAT1Sec = g_mbr.dpt[i].partStartSec\
                            + g_dbr[i].rsvdSecCnt;/* FAT1起始扇区等于DBR起始扇区保留扇区数 */
            FatInitArgs_a[i].FirstDirSector = FatInitArgs_a[i].FAT1Sec\
                             + (g_dbr[i].numFATs * g_dbr[i].FATSz32);
        }
    }
}

/* 解析DBR */
void YC_FAT_ReadDBR(DBR_t * dbr_n)
{
    DBR_t * dbr = dbr_n;

    unsigned char buffer[PER_SECSIZE];

    /* 若没有MBR扇区，则读取绝对0扇区 */
    if(0 == g_dbr_n)
        usr_read((unsigned char *)buffer,0,PER_SECSIZE);
    /* 读DBR所在扇区 */
    else
        usr_read((unsigned char *)buffer,g_mbr.dpt[0].partStartSec,PER_SECSIZE);

    /* 解析buffer数据 */
    dbr->bytsPerSec = Byte2Value((unsigned char *)(buffer+11),2); /* 每扇区大小，通常为512 */
    dbr->secPerClus = Byte2Value((unsigned char *)(buffer+13),1); /* 每簇扇区数 */  
    dbr->rsvdSecCnt = Byte2Value((unsigned char *)(buffer+14),2); /* 保留扇区数（DBR->FAT1）*/
    dbr->numFATs = Byte2Value((unsigned char *)(buffer+16),1);  /* FAT表数，通常为2 */
    dbr->totSec32 = Byte2Value((unsigned char *)(buffer+32),4); /* 总扇区数 */
    dbr->FATSz32 = Byte2Value((unsigned char *)(buffer+36),4); /* 每个FAT的扇区数，FAT32专用 */
}

/* 解析字符串长度 */
unsigned int YC_StrLen(char *str)
{
    char * p_str = str; unsigned int len = 0;

    /* 当遍历到字符'\0'时停止 */
    while( '\0' != (*p_str))
    {
        len ++; p_str ++;
    }
    return len;
}

/* 文件名匹配，返回文件名长度 */
unsigned char ycFilenameMatch(char *fileToMatch,char *fileName)
{
    unsigned char len1 = YC_StrLen(fileName);
    unsigned char len2 = YC_StrLen(fileToMatch);

    if(len1 != len2) return 0;
    unsigned char f_c = 0;
    while( (*(fileToMatch + f_c) == *(fileName + f_c)) && ( f_c < len1) )
    {
        f_c ++;
    }
    if(f_c < (len1 - 1)) return 0;
    return len1;
}

#define EXTNAME_EMP(x) ((x[8] == ' ')&&(x[9] == ' ')&&(x[10] == ' '))

/* 8*3文件名转化为字符串类型 */
/* ycfat中规定文件名不含空格 */
void FDI_FileNameToString(char *from, char *to)
{
    if((from == NULL) || (to == NULL))
        return;

    char *fn_fr = from;
    char *fn_to = to;
    unsigned char i = 0;

    /* 提取文件名 */
    while( ' ' != (*fn_fr))
    {
        *(fn_to + i) = (* fn_fr);
        if( 7 == i) break;
        fn_fr ++; i ++;
    }

    if(EXTNAME_EMP(from))
    {
        *(fn_to + (++i) - 1) = '\0';
        return;
    }
    *(fn_to + (++i) - 1) = '.';

    /* 提取扩展名 */
    fn_fr = from + 8;
    unsigned char j = 0;
    while(' ' != (*fn_fr))
    {
        *(fn_to + i) = (* fn_fr);
        if(2 == j) break;
        fn_fr ++; i ++; j ++;
    }
    *(fn_to + i + 1) = '\0';
}

/* 日期(年-月-日)掩码 */
#define DATE_YY_BASE 1980
#define MASK_DATE_YY 0xFE00
#define MASK_DATE_MM 0x01E0
#define MASK_DATE_DD 0x001F
#define MASK_TIME_HOUR 0xF800
#define MASK_TIME_MIN  0x07E0
#define MASK_TIME_SEC  0x001F

/* 解析文件信息 */
void YC_FAT_AnalyseFDI(FDI_t *fdi,FILE *fileInfo)
{
    FILE * flp = fileInfo;

    /* 文件起始簇号 */
    unsigned int fl_clus = 0;
    fl_clus =  Byte2Value((unsigned char *)&fdi->startClusLower,2);
    fl_clus |=  (Byte2Value((unsigned char *)&fdi->startClusUper,2) << 16);

    /* 解析文件创建日期 */
    typedef struct {J_UINT8 hour;J_UINT8 min;J_UINT8 sec;} fl_crtTime;
    typedef struct {J_UINT16 yy;J_UINT8 mm;J_UINT8 dd;} fl_crtDate;
    fl_crtTime time = {0}; fl_crtDate date = {0};
    unsigned short o_date = (Byte2Value(((unsigned char *)&fdi->crtDate),2));
    date.yy =  DATE_YY_BASE + ( (MASK_DATE_YY & o_date)>>9 );
    date.mm =  (MASK_DATE_MM & o_date) >> 5;
    date.dd = MASK_DATE_DD & o_date;

    /* 解析文件创建时间 */
    unsigned short o_time = (Byte2Value(((unsigned char *)&fdi->crtTime),2));
    time.hour =  (MASK_TIME_HOUR & o_time)>>11 ;
    time.min =  (MASK_TIME_MIN & o_time) >> 5;
    time.sec = 2 * (MASK_TIME_SEC & o_time);

    /* 解析文件最近访问日期 */
    fl_crtDate a_date = {0};
    unsigned short acd = (Byte2Value(((unsigned char *)&fdi->acsDate),2));
    a_date.yy =  DATE_YY_BASE + ( (MASK_DATE_YY & acd)>>9 );
    a_date.mm =  (MASK_DATE_MM & acd) >> 5;
    a_date.dd = MASK_DATE_DD & acd;

    /* 解析文件大小,精确到字节 */
    unsigned int f_s = Byte2Value((unsigned char *)&fdi->fileSize,4);

    /* 将读出的文件信息保存 */
    flp->CurClus = fl_clus;
    flp->CurOffSec = flp->CurOffByte = 0;
    flp->fl_sz = flp->left_sz = f_s ;
    flp->FirstClu = fl_clus;
}

/* 解析根目录簇文件目录信息 */
/* 测试用例，通过 */
SeekFile YC_FAT_ReadFileAttribute(FILE * file,char *filename)
{
    char fileToMatch[13]; /* 最后一字节为'\0' */

    /* 获取根目录起始簇（第2簇） */
    /* FAT32中簇号是从2开始 */
    /* 先由DBR计算首目录簇所在扇区，这里默认只有一个DBR */
    unsigned int fileDirSec;
    if(!g_dbr_n)
        fileDirSec = g_dbr[0].rsvdSecCnt + (g_dbr[0].numFATs * g_dbr[0].FATSz32);
    else
        fileDirSec = g_mbr.dpt[0].partStartSec + g_dbr[0].rsvdSecCnt + (g_dbr[0].numFATs * g_dbr[0].FATSz32);

    FirstDirSector = fileDirSec;
    
    unsigned int fdi_clu = ROOT_CLUS;

    /* 读取首目录簇下的所有扇区 */
    FDIs_t fdis;
    do{
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(fdi_clu)+i,PER_SECSIZE);

            /* 从buffer进行文件名匹配 */
            FDI_t *fdi = NULL;
            fdi = (FDI_t *)&fdis.fdi[0];

            /* 先检查文件属性是否为目录 */
            if( 1 ) /* 不是目录且没有删除 */
            {
                /* 从当前扇区地址循环偏移固定字节取文件名 */
                for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
                {   
                    if( (0x10 != CHECK_FDI_ATTR(fdi)) && (0xE5 != fdi->fileName[0]) )
                    {
                        /* 将目录簇中的8*3文件名转化为字符串类型 */
                        FDI_FileNameToString((char *)fdi->fileName, fileToMatch);

                        /* 匹配到文件名 */
                        if( ycFilenameMatch(fileToMatch,filename) )
                        {
                            YC_FAT_AnalyseFDI(fdi,file);
                            file->file_state = FILE_OPEN; return FOUND;
                        }
                    }
                }
            }
        }
        fdi_clu = YC_TakefileNextClu(fdi_clu);
    }while(!IS_EOF(fdi_clu));
    
    return NOTFOUND;
}

/* 从第n簇（目录起始簇）解析目录簇链文件目录信息 */
SeekFile YC_FAT_MatchFile(unsigned int clu,FILE * file,char *filename)
{
    char DirToMatch[13]; /* 最后一字节为'\0' */
    unsigned int fdi_clu = clu;
    if((NULL == file) || (NULL == filename))
        return NOTFOUND;

    /* 读取首目录簇下的所有扇区 */
    FDIs_t fdis;
    do{
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(fdi_clu)+i,PER_SECSIZE);

            /* 从buffer进行文件名匹配 */
            FDI_t *fdi = NULL;
            fdi = (FDI_t *)&fdis.fdi[0];

            /* 先检查文件属性是否为目录 */
            if( 1 ) /* 不是目录且没有删除 */
            {
                /* 从当前扇区地址循环偏移固定字节取目录名 */
                for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
                {   
                    if( (0x10 != CHECK_FDI_ATTR(fdi)) && (0xE5 != fdi->fileName[0]) )
                    {
                        /* 将目录簇中的8*3文件名转化为字符串类型 */
                        FDI_FileNameToString((char *)fdi->fileName, DirToMatch);

                        /* 匹配到目录名 */
                        if( ycFilenameMatch(DirToMatch,filename) )
                        {
                            YC_FAT_AnalyseFDI(fdi,file);
                            file->file_state = FILE_OPEN;
                            return FOUND;
                        }
                    }
                }
            }
        }
        fdi_clu = YC_TakefileNextClu(fdi_clu);
    }while(!IS_EOF(fdi_clu));
    
    return NOTFOUND;
}

typedef struct FAT_Table
{
    J_UINT8 fat[FAT_SIZE];
}FAT32_t;

typedef struct FAT_TableSector
{
    FATFAT32_t_t fat_sec[PER_SECSIZE/FAT_SIZE];//128
}FAT32_Sec_t;

#define READ_OPS

/* 获取文件下一簇簇号 */
unsigned int YC_TakefileNextClu(unsigned int fl_clus)
{
    unsigned int fat_n = 0;
    FAT32_Sec_t fat_sec;

    /* 解析文件首簇在FAT表中的偏移 */
    /* 先计算总偏移 */
    unsigned int off_b = fl_clus * FAT_SIZE;
    /* 再计算扇区偏移,得到FAT所在绝对扇区 */
    unsigned int off_sec = off_b / PER_SECSIZE;
    unsigned int t_rSec = off_sec + FatInitArgs_a[0].FAT1Sec; /* 默认取DBR0中的数据 */

    /* 取当前扇区所有FAT */
    usr_read((unsigned char *)&fat_sec,t_rSec,PER_SECSIZE);

    FAT32_t * fat = (FAT32_t * )&fat_sec.fat_sec[0];
    unsigned char off_fat = (off_b % PER_SECSIZE)/4;/* 计算在FAT中的偏移（以FAT大小为单位） */
    fat += off_fat;

    /* 返回下一FAT */    
    return fat_n = Byte2Value((unsigned char *)fat,FAT_SIZE);
}

/************************************************/
/* 读取文件整条簇链（文件所有数据在所遍历的簇链中） */
/* 传入参数：文件首簇                            */
/* 传出参数：文件末簇                            */
/* 效率低下，并且对磁盘寿命有影响                 */
/***********************************************/
/*  测试通过  */
static int TakeFileClusList(unsigned int first_clu)
{
    unsigned int clu = first_clu;

    /* 遍历整条簇链 */
    do 
    {
        /* 添加打印信息 */
        //YC_FAT_Printf("%d\r\n",clu);
        clu = YC_TakefileNextClu(clu);
    }while(!IS_EOF(clu));

    return clu;
}

/************************************************/
/* 读取文件整条簇链（文件所有数据在所遍历的簇链中） */
/* 传入参数：文件首簇                            */
/* 传出参数：文件末簇                            */
/* 效率较高，有效降低磁盘读写次数                 */
/***********************************************/
static int TakeFileClusList_Eftv(unsigned int first_clu)
{
    unsigned int clu = first_clu;

    /* 遍历整条簇链 */
    do 
    {
        /* 添加打印信息 */
        //YC_FAT_Printf("%d\r\n",clu);
        clu = YC_TakefileNextClu(clu);
    }while(!IS_EOF(clu));

    return clu;
}

/* 跨扇区？ */
#define READ_EOS(f) (0 == (f->fl_sz-f->left_sz)%PER_SECSIZE)
//#define READ_EOC(f) (0 == (f->fl_sz-f->left_sz)%PER_SECSIZE)

static unsigned char app_buf[PER_SECSIZE];
static unsigned int bk = 0;
/* 待测 */
/* 数据读取函数，不考虑参数len长度可能导致的数据越界 */
J_UINT32 YC_ReadDataNoCheck(FILE* fileInfo,unsigned int len,unsigned char * buffer)
{
    if(FILE_OPEN != fileInfo->file_state)
        return 0;
    FILE1 * f_r;
    unsigned int l_ilegal = 0;  /* 已读的有效数据长度 */
    char i;
    unsigned int n_clu = fileInfo->CurClus; /* 初始簇 */
    unsigned int t_rSize = MIN(len, fileInfo->left_sz);/* 需要读的数据大小 */
	unsigned int t_rb = t_rSize;/* 备份 */
    unsigned int t_rSec = 0, t_rClu = 0;
    unsigned int Secleft = 0;
	
    if(!t_rSize) return 0;
	/* 需要读的扇区个数 */
	t_rSec = ( ( PER_SECSIZE*((fileInfo->fl_sz - fileInfo->left_sz + t_rSize)/PER_SECSIZE) )\
        - ( PER_SECSIZE*((fileInfo->fl_sz - fileInfo->left_sz)/PER_SECSIZE) ) )/PER_SECSIZE;
    if((fileInfo->fl_sz - fileInfo->left_sz + t_rSize)%PER_SECSIZE)
    {
		t_rSec += 1;
    }
    /* 需要读的簇个数 */
	t_rClu = ( ( PER_SECSIZE*g_dbr[0].secPerClus*((fileInfo->fl_sz - fileInfo->left_sz + t_rSize)/(PER_SECSIZE*g_dbr[0].secPerClus)) )\
        - ( PER_SECSIZE*g_dbr[0].secPerClus*((fileInfo->fl_sz - fileInfo->left_sz)/(PER_SECSIZE*g_dbr[0].secPerClus)) ) )/(PER_SECSIZE*g_dbr[0].secPerClus);
    if((fileInfo->fl_sz - fileInfo->left_sz + t_rSize)%(PER_SECSIZE*g_dbr[0].secPerClus))
    {
		t_rClu += 1;
    }
    //t_rClu --;
#if 1    /* 扇区读 */
    /* 找出当前簇内的首扇区（扇区偏移） */
    if(t_rSec)
        Secleft =  g_dbr[0].secPerClus - fileInfo->CurOffSec;
    Secleft = (Secleft <= t_rSec)?Secleft : t_rSec;
    do 
    {
        if(Secleft)
		{
            /* 下面的逻辑都是当前簇读 */
            for(i = 0; i < Secleft; i ++)
            {
                /* 取当前扇区数据 */
				SDCARD_ReadBlocks(&stcSdhandle,START_SECTOR_OF_FILE(n_clu)+fileInfo->CurOffSec , 1,app_buf, PER_SECSIZE);
                //memcpy((unsigned char *)buffer+l_ilegal,app_buf+fileInfo->CurOffByte,MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize));
				memset((unsigned char *)buffer,0,PER_SECSIZE);
				memcpy((unsigned char *)buffer,app_buf+fileInfo->CurOffByte,MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize));
				
				l_ilegal += MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize);      /* 迄今所读出的有效数据大小 */
				
				if(t_rSize <= PER_SECSIZE) bk = t_rSize;
				
                t_rSize = t_rSize - MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize);/* 剩余数据的总大小 */
                
                if(!t_rSize){ 
					/* 锚定起始字节 */
					fileInfo->CurOffByte += bk;
					if(PER_SECSIZE == fileInfo->CurOffByte)
					{
						fileInfo->CurOffSec ++;
						if(g_dbr[0].secPerClus-1 == fileInfo->CurOffSec) 
							fileInfo->CurOffSec = 0;
						fileInfo->CurOffByte = 0;
					}
                    break;
                }
                /* 重新锚定起始字节 */
                fileInfo->CurOffByte = 0;
				/* 重新锚定起始扇区 */
				fileInfo->CurOffSec ++;
				if(g_dbr[0].secPerClus-1 == fileInfo->CurOffSec) 
					fileInfo->CurOffSec = 0;
            }
			
            /* 重新锚定起始簇 */
            t_rSec = t_rSec - Secleft;
            if(t_rSec){//剩下的需要读的总扇区大于0
                n_clu = YC_TakefileNextClu(n_clu);
                fileInfo->CurClus = n_clu;
                Secleft = (t_rSec >= g_dbr[0].secPerClus)?(g_dbr[0].secPerClus):(t_rSec);
            }else{
                break;
            }
            /* 重新锚定起始扇区 */
            fileInfo->CurOffSec = START_SECTOR_OF_FILE(n_clu);
        }
		else
		{
            break;
        }
    }while(!IS_EOF(n_clu));
    /* 可读扇区=0 进行边界处理 */
    if(READ_EOS(fileInfo) && (fileInfo->left_sz == 0)){
        fileInfo->CurClus = fileInfo->FirstClu;
        fileInfo->CurOffSec = 0;
    }else if(1){
        ;
    }
#else    /* 簇读 */
	
#endif
	fileInfo->left_sz -= t_rb;
}

/* 小写转大写 */
J_UINT8 Lower2Up(J_UINT8 ch)
{
    J_UINT8 Upch = 0xff;
    if((ch >= 0x61) && (ch <= 0x7a))
         Upch = *((J_UINT8 *)&ch) - 0x20;
    return Upch;
}

/* 大写转小写 */
J_UINT8 Up2Lower(J_UINT8 ch)
{
    J_UINT8 Lowch = 0xff;
    if((ch >= 0x41) && (ch <= 0x5a))
         Lowch = *((J_UINT8 *)&ch) + 0x20;
    return Lowch;
}

/* 子字符串定位 */
J_UINT8 FindSubStr(char *str,char *substr,unsigned char pos)
{
    J_UINT8 i = pos,j = 0,lens = YC_StrLen(str),lent = YC_StrLen(substr);
    while((i < lens) && (j < lent))
    {
        if((str[i] == substr[j]) || ('?' == substr[j])){
            i ++; j ++;
        }
        else{
            i = i - j + 1; j = 0;
        }
    }
    if(j == lent) 
        return (i - lent);
    else 
        return 0xff;
}

/* 文件通配 */
J_UINT8 FileNameMatch(unsigned char *s,unsigned char *t)
{
    J_UINT8 i = 0, j = 0, lens = YC_StrLen(s), lent = YC_StrLen(t), flag = 0;
    J_UINT8 buf[10];
    J_UINT8 bufp = 0;
    while((j < lent) && ('*' != t[j]))
    {
        buf[bufp] = Lower2Up(t[j]);
        /* 大小写转化失败 */
        if(buf[bufp] == 0xff) return 1;
        bufp ++; j ++;
    }
    buf[bufp] = '\0';
    if(FindSubStr(s, buf, 0) != 0) return 0;
    i = bufp;
    while(1)
    {
        while((j < lent) && ('*' == t[j])) j ++;
        if(j == lent) return 1;
        bufp = 0;
        while((j < lent) && ('*' != t[j]))
        {
            buf[bufp] = Lower2Up(t[j]);
            /* 大小写转化失败 */
            if(buf[bufp] == 0xff) return 1;
            bufp ++; j ++;
        }
        buf[bufp] = '\0';
        if(j == lent)
        {
            if(FindSubStr(s, buf, i) != (lens - bufp)) return 0;
            return 1;
        }
        i = FindSubStr(s, buf, i);
        if(0xff == i) return 0;
        i += bufp;
    }
}

/* 文件路径预处理，剔除无效空格字符 */
void DelexcSpace(char * s,char * d)
{
    int i = 0,j = 0;
    int sp_l = 0;
    char * s1 = s;
    while(' ' == *s1)
    {
        s1 += 1; sp_l++;
    }
    for(i = sp_l;i < YC_StrLen(s);i++)
    {
        d[j] = s[i];j++;
    }
    d[j] = '\0';
}

/* 字符串复制1 */
void YC_StrCpy(char *_tar, char *_src)
{
	do
	{
		*_tar++ = *_src;
	}
	while (*_src++);
}

/* 字符串复制2 */
void YC_StrCpy_l(char *_tar, char *_src, int len)
{
	do
	{
		*_tar++ = *_src;
        len = len - 1;
	}
	while ((*_src++) && len);
}

/* 内存设置 */
void *YC_Memset(void *dest, int set, unsigned len)
{
	if (dest == NULL || len < 0)
	{
		return NULL;
	}
	char *pdest = (char *)dest;
	while (len-->0)
	{
		*pdest++ = set;
	}
	return dest;
}

/* 字符串自截断 */
/* start是丢弃前start个字节 */
/* 传参source不能被const修饰 */
void YC_SubStr(char *source, int start, int length)
{
    int sourceLength = YC_StrLen(source);
    /* Tip：这里直接定义一个50Bytes的数组不严谨，建议采用malloc机制 */
    char bk[50] = {0};
    if (start < 0 || start >= sourceLength || length <= 0){
        source[0] = '\0';
        return;
    }

    int i, j = 0;
    for (i = start; i < start + length && i < sourceLength; i++){
        bk[j++] = source[i];
    }

    bk[j] = '\0';
    YC_StrCpy(source, bk);
}

/* 从文件路径中匹配文件名 */
char YC_FAT_TakeFN(char * s,char *d)
{
    int i = 0;
    char s_l = YC_StrLen(s);
    char *s_end = s + s_l-1;

    /* 匹配最后一个'/'所在位置，采用后序遍历 */
    while(*s_end)
    {
        if((*s_end == '/')||(*s_end == '\\'))
        {
            if(i == 0)
                return 0;
            break;
        }
        s_end--;i++;
    }
    YC_StrCpy(d, s_end+1);
    return 1;
}

/* 从文件路径中匹配目录名 */
char YC_FAT_TakeFP(char * s,char *d)
{
    int i = 0;
    char s_l = YC_StrLen(s);
    char *s_end = s + s_l-1;

    /* 匹配最后一个'/'所在位置，采用后序遍历 */
    while(*s_end)
    {
        if((*s_end == '/')||(*s_end == '\\'))
        {
            if(i == 0)
                return 0;
            break;
        }
        s_end--;i++;
    }
    YC_StrCpy_l(d, s, s_l-i-1);
    return 1;
}

/* 函数声明 */
unsigned int YC_FAT_EnterDir(char *dir);

/* 打开文件（雏形） */
FILE * YC_FAT_fopen(FILE * f_op, char * filepath)
{
    FILE * file = NULL;
    char fp[50];
    unsigned int file_clu = 0;
    if(f_op->file_state == FILE_OPEN)
        return NULL;

    /* 文件路径预处理 */
    DelexcSpace(filepath,fp);

    /* 传参合法性检查 */
    /* 是否为..///或者.../或者.././//等不合法形式 */

    /* 从路径匹配文件名 */
    char f_n[50] = {0};
    if(!YC_FAT_TakeFN(fp,f_n)) return NULL;

    /* 从路径匹配目录名 */
    char f_p[50] = {0};
    if(!YC_FAT_TakeFP(fp,f_p)) return NULL;

    if(!IS_FILENAME_ILLEGAL(f_p))
        return NULL;

    /* 进入文件目录，这里假设是标准绝对路径寻找文件 */
    file_clu = YC_FAT_EnterDir(f_p);

    if(FOUND == YC_FAT_MatchFile(file_clu,f_op,f_n))
    {
        /* 匹配成功 */
        file = f_op;
        INIT_LIST_HEAD(&file->WRCluChainList);

        /* 找出文件尾簇,写文件用 */
        file->EndClu = TakeFileClusList_Eftv(file->FirstClu);
        
        return file;
    }
    return NULL;
}

/* 读取文件 */
void fread(FILE * f_rd, unsigned int len, void *buffer)
{
    if((NULL == f_rd)||(0 == len)||(NULL == buffer))
        return;
    YC_ReadDataNoCheck(f_rd,len,buffer);
}

/* 关闭文件 */
void fclose(FILE * f_cl)
{
    if(NULL == f_cl) return;
    f_cl->CurClus = f_cl->CurOffByte = f_cl->CurOffSec = 0;
    f_cl->file_state = FILE_CLOSE;
    f_cl->FirstClu = 0;
    f_cl->fl_sz = 0;
    f_cl->left_sz = 0;
}

/* 从第n号簇（某一目录开始簇）开始匹配目录，并返回目录首簇 */
/* 配合enterdir函数使用 */
unsigned int YC_FAT_MatchDirInClus(unsigned int clu,char *DIR)
{
    char DirToMatch[13]; /* 最后一字节为'\0' */
    unsigned int fdi_clu = clu;
    /* 目录起始簇号 */
    unsigned int dir_clu = 0;

    /* 读取首目录簇下的所有扇区 */
    FDIs_t fdis;
    do{
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(fdi_clu)+i,PER_SECSIZE);

            /* 从buffer进行文件名匹配 */
            FDI_t *fdi = NULL;
            fdi = (FDI_t *)&fdis.fdi[0];

            /* 先检查文件属性是否为目录 */
            if(1) /* 是目录且没有删除 */
            {
                /* 从当前扇区地址循环偏移固定字节取目录名 */
                for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
                {   
                    if( (0x10 != CHECK_FDI_ATTR(fdi)) && (0xE5 != fdi->fileName[0]) )
                    {
                        /* 将目录簇中的8*3名转化为字符串类型 */
                        FDI_FileNameToString((char *)fdi->fileName, DirToMatch);

                        /* 匹配到目录名 */
                        if( ycFilenameMatch(DirToMatch,DIR) )
                        {
                            dir_clu =  Byte2Value((unsigned char *)&fdi->startClusLower,2);
                            dir_clu |=  (Byte2Value((unsigned char *)&fdi->startClusUper[1],2) << 16);
                            return dir_clu;
                        }
                    }
                }
            }
        }
        fdi_clu = YC_TakefileNextClu(fdi_clu);
    }while(!IS_EOF(fdi_clu));
    
    return 0;
}

/* 获取当前工作目录 */
void YC_FAT_getPWD(unsigned char fsn,unsigned char * p)
{
    for(int i = 0;i < YC_StrLen(&pwd[fsn]);i ++)
        {*p = (pwd[fsn][i]); p ++;}
}

/* 目录解析，保存'/'或者'\\'之后的第一个目录 */
/* 将dir_s下的第一个/（斜杠）或者\\（反斜杠）后的子目录 */
/* 斜杠的ascii码为0x2f(47)，用'/'表示，占用一个字节*/
/* 反斜杠的ascii码为0x5c(92)，用'\\'表示，占用一个字节 */
/* 根目录没有./和../目录项，根目录下的目录的../项的簇号为2，也就是根目录簇号为2 */
/* 输入为标准的/dir1/dir2/dir3格式或者.././dir/dir1/.等格式 */
unsigned char YC_FAT_ParseDir(char * dir_s,char * dir_d)
{
    int i = 0;char *p;
    int len = YC_StrLen(dir_s);
    if(
        ( '/' == (*(dir_s)) ) || ( '\\' == (*(dir_s+1)) )
    )
    {
        p = dir_s+1;
        while((*p != '\0')&&(*p != '/')&&(*p != '\\'))
        {
            dir_d[i++] = *p;
            p++;
        }
        dir_d[i] = '\0';
    }
    else if('.' == *dir_s){
        p = dir_s;
        while((*p != '\0')&&(*p != '/')&&(*p != '\\'))
        {
            dir_d[i++] = *p;
            p++;
        }
        dir_d[i] = '\0';
        return i-1;
    }
    return i;
}

static unsigned int yc_fstick = 0;
unsigned int YC_TakeSystick(void)
{   
    return yc_fstick;
}

#define IS_TIMEOUT(n_t,tot) ((n_t-tot)>yc_fstick)

/* 目录索引错误码 */
#define ENTER_ROOT_PDIR_ERROR -1
#define ENTER_DIR_TIMEOUT_ERROR -2

/* 按层级进入目录 */
unsigned int YC_FAT_EnterDir(char *dir)
{
    unsigned int dir_clu = 0xffffffff;

    char dir_temp[20] = {0};
    unsigned char i = 0;

    /* 锚定起始目录簇 */
    if((*(char *)fp == '.')&&(*((char *)fp+1)) == '..'))
        dir_clu = work_clu[0];
    else if((*(char *)fp == '\\')||(*(char *)fp == '/'))  {
        dir_clu = ROOT_CLUS;
    }
    else if(*(char *)fp = '.')  
        dir_clu = work_clu[0];

#if YC_TIMEOUT_SWITCH
    int tick_now = YC_TakeSystick();
#endif

    /* 递归式遍历子目录 */
    for( ; ; )
    {
        if(!YC_StrLen(dir))
            break;
        {
            i = YC_FAT_ParseDir(dir,dir_temp);
            if(((ROOT_CLUS == dir_clu) && ('.' == *dir_temp) && ('.' == *(dir_temp+1)))){
                return ENTER_ROOT_PDIR_ERROR;/* 非法目录 */
            }
            dir_clu = YC_FAT_MatchDirInClus(dir_clu,dir_temp);
            YC_SubStr(dir, i+1, 100);
        }
        /* 遍历超时退出，返回错误码 */
#if YC_TIMEOUT_SWITCH
        if(IS_TIMEOUT(tick_now,1000));
            return ENTER_DIR_TIMEOUT_ERROR;
#endif
    }
    return dir_clu;
}

/* CD脚本 */
/* 待测试 */
unsigned int YC_CD(char *dir)
{
    unsigned int cc = 0xffffffff;
    cc = YC_FAT_EnterDir(dir);

    if(cc != 0xffffffff)
        work_clu[0] = cc;
    return cc;
}

/* 更新FSINFO扇区，主要用于更新剩余空闲簇数目 */
void YC_FAT_UpdateFSInfo(void)
{
    FSINFO_t fsi,* pfsi = &fsi;
    usr_read((unsigned char *)&fsi,g_mbr.dpt[0].partStartSec+1,PER_SECSIZE);
    pfsi->Free_nClus[0] = FatInitArgs_a[0].FreeClusNum;
    pfsi->Free_nClus[1] = FatInitArgs_a[0].FreeClusNum>>8;
    pfsi->Free_nClus[2] = FatInitArgs_a[0].FreeClusNum>>16;
    pfsi->Free_nClus[3] = FatInitArgs_a[0].FreeClusNum>>24;
    usr_write((char *)&fsi,g_mbr.dpt[0].partStartSec+1,PER_SECSIZE);
}

/* 读取FSINFO扇区 */
void YC_FAT_ReadInfoSec(unsigned int *leftnum)
{
    FSINFO_t fsinfo;
    usr_read((unsigned char *)&fsinfo,g_mbr.dpt[0].partStartSec+1,PER_SECSIZE);
    FatInitArgs_a[0].FreeClusNum = Byte2Value((unsigned char *)&fsinfo.Free_nClus,4);
}

/* 遍历FAT表，寻找第一个空簇 */
int YC_FAT_SeekFirstEmptyClus(unsigned int * d)
{
    /* 遍历FAT所有扇区 */
    /* 由DBR获取FAT首扇区地址 */
    int j = g_dbr[0].FATSz32;int k;
    unsigned int fat_ss = g_mbr.dpt[0].partStartSec+g_dbr[0].rsvdSecCnt;
    FAT32_Sec_t fat_secA;
    FAT32_t * fat;
    for(k = 0; k < j; k++)
    {
        /* 取当前扇区所有FAT链 */
        usr_read((unsigned char *)&fat_secA,fat_ss+k,PER_SECSIZE);
		fat = (FAT32_t *)&fat_secA.fat_sec[0];
        for(; (unsigned int)fat < ((unsigned int)&fat_secA+sizeof(FAT32_Sec_t)); fat++)
        {
            /* 找到一个FAT */
            if(0x00 == *(unsigned int *)fat) {
                /* 将在一个扇区内的Byte偏移转化为簇号 */
                *(unsigned int *)d = k*(PER_SECSIZE/FAT_SIZE)+((unsigned int)fat-(unsigned int)&fat_secA)/FAT_SIZE;
                return 0;
            }
            else continue;
        }
    }
    /* cannot find empty clus */
    return -1;
}

#define CLR_BIT(a,n) (a = a&(~(1<<n)))
#define SET_BIT(a,n) (a = a|(1<<n))

/* FAT表映射到位图,默认1个扇区的FAT */
int YC_FAT_RemapToBit(unsigned int start_sec)
{
    FAT32_Sec_t fat_secA;
    unsigned int *pi = (unsigned int *)&fat_secA;
    unsigned char *pc = clusterBitmap;
    unsigned char n = 0,k = 0;
    YC_Memset(clusterBitmap, 0, sizeof(clusterBitmap));
    /* 先读出FAT扇区所有数据 */
    usr_read((unsigned char *)&fat_secA,start_sec,1);
    /* 将整个FAT扇区映射到位图，0->0,!0->1 */
    while((unsigned int)pi < ((unsigned int)&fat_secA + PER_SECSIZE))
    {
        if((*pi)&&(0xffffffff)){
            SET_BIT(*pc,n);k++;
        }
        /*else
            CLR_BIT(*pc,n);*/
        if(n == 7)
        {
            pc++;
            n = 0;
        }else{
            n++;
        }
        pi++;
    }
    /* 无空闲簇，返回错误码 */
    if(PER_SECSIZE/FAT_SIZE == k)
        return -1;
    return 0;
}

/* 扩展簇链（不进行自动缝合簇链） */
int YC_FAT_ExpandCluChain(unsigned int theclu,unsigned int nextclu)
{
    /* 索引theclu在FAT表中的偏移 */
    FAT32_Sec_t fat_sec1;

    /* 解析文件首簇在FAT表中的偏移 */
    /* 先计算总偏移 */
    unsigned int off_b = theclu * FAT_SIZE;
    /* 再计算扇区偏移,得到FAT所在绝对扇区 */
    unsigned int off_sec = off_b / PER_SECSIZE;
    unsigned int t_rSec = off_sec + FatInitArgs_a[0].FAT1Sec; /* 默认取DBR0中的数据 */

    /* 取当前扇区所有FAT */
    usr_read((unsigned char *)&fat_sec1,t_rSec,PER_SECSIZE);

    FAT32_t * fat = (FAT32_t * )&fat_sec1.fat_sec[0];
    unsigned char off_fat = (off_b % PER_SECSIZE)/4;/* 计算在FAT中的偏移（以FAT大小为单位） */
    fat += off_fat;

    /* 修改此簇的下一簇为nextclu */    
    *(unsigned char *)(fat) = nextclu;
    *((unsigned char *)(fat)+1) = nextclu >> 8;
    *((unsigned char *)(fat)+2) = nextclu >> 16;
    *((unsigned char *)(fat)+3) = nextclu >> 24;

    /* 回写扇区 */
    usr_write((unsigned char *)&fat_sec1,t_rSec,PER_SECSIZE);
    return 0;
}

#define FOUND_FREE_CLU 0
#define NO_FREE_CLU -1

/* 寻找当前簇的下一个空闲簇 */
/* 简单测试通过 */
int YC_FAT_SeekNextFirstEmptyClu(unsigned int current_clu,unsigned int * free_clu)
{
    if(!free_clu) return ARGVS_ERROR;

    FAT32_Sec_t fat_sec1;FAT32_t * fat;
    current_clu ++;
    /* 是否存在满足需求的空簇 */
    if(!FatInitArgs_a[0].FreeClusNum) return NO_FREE_CLU;
    /* 从当前FAT表所在扇区向后遍历FAT表中的所有扇区，找出第一个空闲簇 */
    unsigned int t_rSec = (current_clu * FAT_SIZE / PER_SECSIZE) + FatInitArgs_a[0].FAT1Sec;
    for(;t_rSec < FatInitArgs_a[0].FAT1Sec + g_dbr[0].FATSz32;t_rSec ++)
    {
        /* 取当前扇区所有FAT */
        usr_read((unsigned char *)&fat_sec1,t_rSec,PER_SECSIZE);
        fat = (FAT32_t * )&fat_sec1.fat_sec[0];
        fat = fat + (current_clu * FAT_SIZE % PER_SECSIZE)/4;
        /* 从当前FAT所在扇区偏移开始向后遍历 */
        for(; (unsigned int)fat < ((unsigned int)&fat_sec1+sizeof(FAT32_Sec_t)); fat++)
        {
            current_clu ++;
            /* 找到一个FAT为0 */
            if(0 == *(unsigned int *)fat) {
                /* 将在一个扇区内的Byte偏移转化为簇号 */
                *(unsigned int *)free_clu = (t_rSec-FatInitArgs_a[0].FAT1Sec)*(PER_SECSIZE/FAT_SIZE)+\
                                            ((unsigned int)fat-(unsigned int)&fat_sec1)/FAT_SIZE;
                return 0;
            }
        }
    }
    /* 若遍历完，还未找到空簇，从头开始遍历 */
    if(-1 == YC_FAT_SeekFirstEmptyClus(free_clu))
        return -1;
    return FOUND_FREE_CLU;
}

/* ycfat初始化 */
void YC_FAT_Init(void)
{
    /* 解析绝对0扇区 */
    YC_FAT_AnalyseSec0();
    
    /* 解析DBR */
    YC_FAT_ReadDBR(&g_dbr[0]);

    /* 获取FAT表大小推荐参数 */
    //unsigned int disk_size = ioctl();

    /* 遍历FAT表，寻找第一个空闲簇 */
    YC_FAT_SeekFirstEmptyClus((unsigned int *)&FatInitArgs_a[0].NextFreeClu);
    /* 第一个空闲簇所在FAT扇区 */
    cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);

    /* 找出第一个有空闲簇的FAT扇区 */
    if((FatInitArgs_a[0].NextFreeClu != 0xffffffff) && (FatInitArgs_a[0].NextFreeClu != 0))
        YC_FAT_RemapToBit(cur_fat_sec);

    /* 读取FSINFO扇区，更新剩余空簇 */
    YC_FAT_ReadInfoSec((unsigned int *)&FatInitArgs_a[0].FreeClusNum);
}

/* ------------------------------------------ */
/*        write operations start here         */
/* ------------------------------------------ */

/* 传参最多含有一个. */
static void Genfilename_s(char *filename,char *d)
{
    if((NULL == d)||(NULL == filename)) return;
    int len = YC_StrLen(filename);
    char *fn = filename;
    char i = 0;char j = 0;
    for(i = 0; i < len; i++){
        if('.' == fn[i])
            break;
    }
    /* 遍历后未发现字符'.' */
    if( len == i ) {
        if(8 >= len){
            YC_StrCpy_l(d,fn,len);
            for(j = len;j <= 7;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d,fn,7);
            d[7] = '~';
        }
        d[8] = d[9] = d[10] = ' ';
    }
    /* 在串尾发现字符'.' */
    else if((len - 1) == i){
        if(i <= 8){
            YC_StrCpy_l(d,fn,i);
            for(j = i;j <= 7;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d,fn,7);
            d[7] = '~';
        }
        d[8] = d[9] = d[10] = ' ';
    }
    /* 串头发现字符'.' */
    else if(!i){
        /* 直接返回，不做处理 */
        return;
    }
    /* 标准*.*格式或者多.格式 */
    else{
        if(i <= 8){
            YC_StrCpy_l(d,fn,i);
            for(j = i;j <= 7;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d,fn,7);
            d[7] = '~';
        }
        if((len-i-1) <= 3){
            YC_StrCpy_l(d+8,fn+i+1,len-i-1);
            for(j=(len-i+7);j <= 10;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d+8,fn+i+1,2);
            d[10] = '~';
        }
    }
}

#define MAKETIME(T) 
#define MAKEDATE(T)

typedef enum CreatFDItype
{
    FDIT_DIR = 0,
    FDIT_FILE = 1
}FDIT_t;

/* 生成文件目录项fdi */
/* 测试通过 */
static void YC_FAT_GenerateFDI(FDI_t *fdi,char *filename,FDIT_t fdi_t)
{
    /* 检查传参合法性 */
    if((NULL == fdi)||(NULL == filename)||(!IS_FILENAME_ILLEGAL(filename))) return;
    
    FDI_t *fdi2full = fdi;
    char fn[11] = {0};

    /* create SFN */
    if(FDIT_FILE == fdi_t)
        Genfilename_s(filename,fn);
    else{
        if((*filename == '.')&&(*(filename+1) == '.')){
            fn[0] = '.';fn[1] = '.';fn[2]=fn[3]=fn[4]=fn[5]=fn[6]=fn[7]=fn[8]=fn[9]=fn[10] = ' ';
        }else if(*filename == '.'){
            fn[0] = '.';fn[1]=fn[2]=fn[3]=fn[4]=fn[5]=fn[6]=fn[7]=fn[8]=fn[9]=fn[10] = ' ';
        }
        else if((*filename != '.')&&(*(filename+1) != '.')){
            Genfilename_s(filename,fn);
        }
    }

    YC_StrCpy_l((char *)fdi2full->fileName,fn,sizeof(fn));/* fill file name */
    fdi2full->attribute = (FDIT_FILE == fdi_t)?ARCHIVE:TP_DIR;/* 属性字段文件or目录 */
	if(*filename == '.')
		fdi2full->UpLower = 0;
	else
		fdi2full->UpLower = (FDIT_FILE == fdi_t)?0x10:0x08;
    *(J_UINT16 *)fdi2full->startClusUper = 0;
    *(J_UINT16 *)fdi2full->startClusLower = 0;
    *(J_UINT32 *)fdi2full->fileSize = 0;
#if YC_TIMESTAMP_ON
    *(J_UINT16 *)fdi2full->crtTime = MAKETIME(systime_now());
    *(J_UINT16 *)fdi2full->crtDate = MAKEDATE(sysdate_now());
#else
    *(J_UINT16 *)fdi2full->crtTime = 0;
    *(J_UINT16 *)fdi2full->crtDate = 0;
#endif
}

#define CRT_FILE_OK 0
#define CRT_SAME_FILE_ERR -1
#define CRT_FILE_NO_FREE_CLU_ERR -2

/* create file operation */
int YC_FAT_CreateFile(char *filepath)
{
    if(NULL == filepath)
        return;
    unsigned int file_clu = 0; char f_n[50] = {0}; char f_p[50] = {0};
    char fp[50];
    char FileToMatch[13]; /* 最后一字节为'\0' */
    unsigned int tail_clu = 0;

    /* 文件路径预处理 */
    DelexcSpace(filepath,fp);
    
    if(!YC_FAT_TakeFN(fp,f_n)) return NULL; if(!YC_FAT_TakeFP(fp,f_p)) return NULL;
    if(!IS_FILENAME_ILLEGAL(f_p)) return NULL;

    /* 进入文件目录，返回首目录簇 */
    file_clu = YC_FAT_EnterDir(f_p);

    FDIs_t fdis; FDI_t *fdi;
    do{
        tail_clu = file_clu;
        /* 遍历簇下所有扇区 */
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,PER_SECSIZE);
            fdi = (FDI_t *)&fdis.fdi[0];
            /* 从当前扇区地址循环偏移固定字节取文件/目录名 */
            for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
            {   
                if(0x00 == *(char *)fdi) 
                {
                    YC_FAT_GenerateFDI(fdi,f_n,FDIT_FILE);
                    /* 回写当前扇区并退出 */
                    usr_write((char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,PER_SECSIZE);
                    return CRT_FILE_OK;
                }
                /* 将目录簇中的8*3名转化为字符串类型 */
                FDI_FileNameToString((char *)fdi->fileName, FileToMatch);
                /* 同名文件 返回错误码 */
                if(ycFilenameMatch(FileToMatch,f_n)) return CRT_SAME_FILE_ERR;
            }
        }
        file_clu = YC_TakefileNextClu(file_clu);
    }while(!IS_EOF(file_clu));

    /* 当前簇空间不足，寻找空簇扩展目录簇链 */
    /* 寻找第一个空闲簇 */
    unsigned int freeclu = FatInitArgs_a[0].NextFreeClu;

    /* 若没有空闲簇，错误返回 */
    if(0xffffffff == freeclu)
        return CRT_FILE_NO_FREE_CLU_ERR;

    /* 扩展目录簇链 */
    YC_FAT_ExpandCluChain(tail_clu,freeclu);
    YC_FAT_ExpandCluChain(freeclu,0xffffffff);

    /* 在新簇头部写入新fdi */
    usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),PER_SECSIZE);
    fdi = (FDI_t *)&fdis.fdi[0];
    YC_FAT_GenerateFDI(fdi,f_n,FDIT_FILE);
    usr_write((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),PER_SECSIZE);
    
    /* 更新FSINFO扇区中的空簇数目 */
    FatInitArgs_a[0].FreeClusNum --;
    YC_FAT_UpdateFSInfo();
    /* 寻找下一空闲簇 */
    if(FatInitArgs_a[0].FreeClusNum)
        YC_FAT_SeekNextFirstEmptyClu(freeclu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);

    return CRT_FILE_OK;
}

#define CRT_DIR_OK 0
#define CRT_SAME_DIR_ERR -1
#define CRT_DIR_NO_FREE_CLU_ERR -2
/* 在当前簇下创建新目录，p_clu是新目录的父目录簇号 */
int YC_GenDirInClu(unsigned int thisclu,unsigned int p_clu)
{
    FDIs_t fdis; FDI_t *fdi = (FDI_t *)&fdis;
	YC_Memset((char *)&fdis,0,sizeof(FDIs_t));
    YC_FAT_GenerateFDI(fdi,".",FDIT_DIR);
	fdi->startClusUper[0] = thisclu >> 16;
    fdi->startClusUper[1] = thisclu >> 24;
    fdi->startClusLower[0] = thisclu;
    fdi->startClusLower[1] = thisclu >> 8;
    
	fdi ++;
    YC_FAT_GenerateFDI(fdi,"..",FDIT_DIR);
	if(ROOT_CLUS != p_clu)
	{
		fdi->startClusUper[0] = p_clu >> 16;
		fdi->startClusUper[1] = p_clu >> 24;
		fdi->startClusLower[0] = p_clu;
		fdi->startClusLower[1] = p_clu >> 8;
	}
    usr_write((unsigned char *)&fdis,START_SECTOR_OF_FILE(thisclu),PER_SECSIZE);
    return 0;
}

/* create directory operation */
int YC_FAT_CreateDir(char *dir)
{
    if(NULL == dir)
        return;
    unsigned int file_clu = 0; char f_n[50] = {0}; char f_p[50] = {0};
    char fp[50];int freeclu;
    char FileToMatch[13]; /* 最后一字节为'\0' */
    unsigned int tail_clu = 0;

    /* 文件路径预处理 */
    DelexcSpace(dir,fp);
    
    if(!YC_FAT_TakeFN(fp,f_n)) return NULL; if(!YC_FAT_TakeFP(fp,f_p)) return NULL;
    if(!IS_FILENAME_ILLEGAL(f_p)) return NULL;

    /* 进入文件目录，返回首目录簇 */
    file_clu = YC_FAT_EnterDir(f_p);

    FDIs_t fdis; FDI_t *fdi;
    do{
        tail_clu = file_clu;
        /* 遍历簇下所有扇区 */
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,PER_SECSIZE);
            fdi = (FDI_t *)&fdis.fdi[0];
            /* 从当前扇区地址循环偏移固定字节取文件/目录名 */
            for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
            {   
                if(0x00 == *(char *)fdi) 
                {
                    if(!FatInitArgs_a[0].FreeClusNum)
						return CRT_DIR_NO_FREE_CLU_ERR;
                    YC_FAT_GenerateFDI(fdi,f_n,FDIT_DIR);
                    fdi->startClusUper[0] = FatInitArgs_a[0].NextFreeClu >> 16;
                    fdi->startClusUper[1] = FatInitArgs_a[0].NextFreeClu >> 24;
                    fdi->startClusLower[0] = FatInitArgs_a[0].NextFreeClu;
                    fdi->startClusLower[1] = FatInitArgs_a[0].NextFreeClu >> 8;

                    usr_write((char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,PER_SECSIZE);
                    YC_FAT_ExpandCluChain(FatInitArgs_a[0].NextFreeClu,0xffffffff);
                    YC_GenDirInClu(FatInitArgs_a[0].NextFreeClu,file_clu);
                    freeclu = FatInitArgs_a[0].NextFreeClu;
                    YC_FAT_SeekNextFirstEmptyClu(freeclu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);
                    
                    /* 更新FSINFO扇区中的空簇数目 */
                    FatInitArgs_a[0].FreeClusNum --;
                    YC_FAT_UpdateFSInfo();
                    return CRT_DIR_OK;
                }
                /* 将目录簇中的8*3名转化为字符串类型 */
                FDI_FileNameToString((char *)fdi->fileName, FileToMatch);

                /* 同名目录 返回错误码 */
                if(ycFilenameMatch(FileToMatch,f_n)) return CRT_SAME_DIR_ERR;
            }
        }
        file_clu = YC_TakefileNextClu(file_clu);
    }while(!IS_EOF(file_clu));

    /* 当前簇空间不足，寻找空簇扩展目录簇链 */
    /* 寻找第一个空闲簇 */
    freeclu = FatInitArgs_a[0].NextFreeClu;

    /* 若没有空闲簇，错误返回 */
    if(0xffffffff == freeclu)
        return CRT_DIR_NO_FREE_CLU_ERR;

    /* 判断剩余空闲簇数目是否足够扩展目录 */
    if(!(FatInitArgs_a[0].FreeClusNum-2))
        return CRT_DIR_NO_FREE_CLU_ERR;
    
    /* 扩展目录簇链 */
    YC_FAT_ExpandCluChain(tail_clu,freeclu);
    YC_FAT_ExpandCluChain(freeclu,0xffffffff);
    YC_FAT_SeekNextFirstEmptyClu(freeclu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);
    /* 在当前目录扩展新簇头部写入新fdi */
    usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),PER_SECSIZE);
    YC_Memset(&fdis, 0, sizeof(FDIs_t));
    fdi = (FDI_t *)&fdis.fdi[0];
    YC_FAT_GenerateFDI(fdi,f_n,FDIT_DIR);
    fdi->startClusUper[0] = FatInitArgs_a[0].NextFreeClu >> 16;
    fdi->startClusUper[1] = FatInitArgs_a[0].NextFreeClu >> 24;
    fdi->startClusLower[0] = FatInitArgs_a[0].NextFreeClu;
    fdi->startClusLower[1] = FatInitArgs_a[0].NextFreeClu >> 8;
    usr_write((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),PER_SECSIZE);

    YC_FAT_ExpandCluChain(FatInitArgs_a[0].NextFreeClu,0xffffffff);
    /* 在子目录新簇写入fdi */
    YC_GenDirInClu(FatInitArgs_a[0].NextFreeClu,tail_clu);

    /* 更新FSINFO扇区中的空簇数目 */
    FatInitArgs_a[0].FreeClusNum -= 2;
    YC_FAT_UpdateFSInfo();
    /* 寻找下一空闲簇 */
    if(FatInitArgs_a[0].FreeClusNum)
        YC_FAT_SeekNextFirstEmptyClu(FatInitArgs_a[0].NextFreeClu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);

    return CRT_DIR_OK;
}

/* 在FAT位图中寻找下一个空簇,找不到下一个空簇就返回-1 */
/* 测试通过 */
int SeekNextFreeClu_BitMap(unsigned int clu)
{
    /* clu在bitmap中的索引 */
    int a = clu%(PER_SECSIZE/FAT_SIZE);
    unsigned char b = a/8;
    char c = a%8;
    unsigned char * p = ((unsigned char *)clusterBitmap + b);
    unsigned int next = 0;
    char k = 0;

    if(c == 7){
        p++;c=-1;
    }
    for(;p < clusterBitmap + sizeof(clusterBitmap); p++)
    {
        if((*p & 0xff) == 0xff)
        {
            c = -1;
            continue;
        }
        else
        {
            for(k=c+1; k<8; k++)
            {
                if (((*p >> k) & 0x01) != 0x01)
                {
                    next = cur_fat_sec*(PER_SECSIZE/FAT_SIZE) +  ((unsigned int)p - (unsigned int)clusterBitmap)*8 + k;
                    return next;
                }
            }
        }
        c = -1;
    }
    /* clu在当前FAT位图向下索引时没有空簇了 */
    return -1;
}

/* 将空簇添加至文件写缓冲簇链中 */
/* 2023.11.22测试通过 */
int YC_FAT_AddToList(FILE *fl,unsigned int clu)
{
    if(NULL == fl) return -1;
    w_buffer_t *w_ccb = NULL;
    struct list_head *pos,*tmp;

    if(list_empty(&fl->WRCluChainList))
    {
        /* 新建第一个节点 */
        w_ccb = (w_buffer_t *)tAllocHeapforeach(sizeof(w_buffer_t));
        if(NULL == w_ccb)
        {
            /* 错误处理，删除并释放所有链表节点 */
            list_for_each_safe(pos, tmp, &fl->WRCluChainList)
            {
                list_del(pos);
                tFreeHeapforeach((void *)pos);
            }
            return -1;
        }
        /* 初始化第一个节点 */
        w_ccb->w_s_clu = w_ccb->w_e_clu = clu;
        list_add_tail(&w_ccb->WRCluChainNode,&fl->WRCluChainList);
    }
    else
    {
        /* 先找到最后一个压缩簇链缓冲节点 */
        w_ccb = (w_buffer_t *)(fl->WRCluChainList.prev);
        /* 新簇与压缩簇链缓冲节点进行匹配,匹配成功则添加至此节点 */
        if(clu == (w_ccb->w_e_clu + 1))
        {
            w_ccb->w_e_clu = clu;
        }
        /* 匹配失败则分配新节点 */
        else
        {
            w_ccb = (w_buffer_t *)tAllocHeapforeach(sizeof(w_buffer_t));
            if(NULL == w_ccb)
            {
                /* 错误处理，删除并释放所有链表节点 */
                list_for_each_safe(pos, tmp, &fl->WRCluChainList)
                {
                    list_del(pos);
                    tFreeHeapforeach((void *)pos);
                }
                return -1;
            }
            w_ccb->w_s_clu = w_ccb->w_e_clu = clu;
            list_add_tail(&w_ccb->WRCluChainNode,&fl->WRCluChainList);
        }
    }
    return 0;
}

/* 2023/11/17注释：基本思路是将含有空簇的FAT表读出来，在调用fwrite时，分配簇，将压缩缓冲簇链记录进mheap中，在save时缝合簇链 */
/* 预建文件簇链缓冲（链表形式），暂存在theap中 */
int YC_FAT_CreateFileCluChain(FILE *fl,unsigned int cluNum)
{
    unsigned int ret = 0;
    /* 还原FatInitArgs_a[0].NextFreeClu备用 */
    unsigned int bkclu = FatInitArgs_a[0].NextFreeClu;
    /* 还原FatInitArgs_a[0].NextFreeClu备用1 */
    unsigned int bkclu1;

    /* 遍历bit map，将0位存放到链表中 */
    while(cluNum--)
    {
        if(-1 == YC_FAT_AddToList(fl,FatInitArgs_a[0].NextFreeClu)) //返回-1表示堆栈空间不足
        {
            /* 还原历史数据 */
            FatInitArgs_a[0].NextFreeClu = bkclu;
            cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);
            YC_FAT_RemapToBit(cur_fat_sec);
            /* 退出,返回错误码 */
            ret = -1;
            break;
        }
        FatInitArgs_a[0].NextFreeClu == SeekNextFreeClu_BitMap(FatInitArgs_a[0].NextFreeClu);
        bkclu1 = FatInitArgs_a[0].NextFreeClu;
        if(-1 == FatInitArgs_a[0].NextFreeClu)
        {
            /* 继续往下找出第一个空闲簇 */
            FatInitArgs_a[0].NextFreeClu = bkclu1;
            YC_FAT_SeekNextFirstEmptyClu(FatInitArgs_a[0].NextFreeClu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);

            /* 继续将下一空闲簇所在FAT扇区映射 */
            cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);
            if((FatInitArgs_a[0].NextFreeClu != 0xffffffff) && (FatInitArgs_a[0].NextFreeClu != 0))
                YC_FAT_RemapToBit(cur_fat_sec);
        }
    }
    return ret;
}

#if PRINT_DEBUG_ON
/* 列出文件所有写压缩缓冲簇链 */
void YC_FAT_ListFileCluChain(FILE *fl)
{
    struct list_head *pos;
    if(!list_empty(&fl->WRCluChainList))
    {
        list_for_each(pos, &fl->WRCluChainList)
        {
            printf("start_clu = %d,end_clu = %d\n",((w_buffer_t *)pos)->w_s_clu,((w_buffer_t *)pos)->w_e_clu);
        }
    }
}
#endif

/* 临时交换区 */
static unsigned int buffer1[PER_SECSIZE];

/* 写文件，只支持在文件末尾追加数据 */
//JYCFAT库只需要向底层提供起始扇区，写扇区数两个参数即可！
//对于多文件并发写入时，采用一些策略来优化簇的分配，确保并发写入的正确性
//策略1：定义互斥量mutex，while(1)等待互斥量释放，只建议独立线程中使用，其他情况不建议使用
//策略2：定义互斥量mutex，非阻塞等待，若互斥量不可用，直接返回错误码
//策略3：定义互斥量mutex，阻塞等待，若互斥量不可用，陷入内核
int YC_WriteDataNoCheck(FILE* fileInfo,unsigned char * d_buf,unsigned int len)
{
    if(NULL == fileInfo)
        return -1;
    if(FILE_OPEN != fileInfo->file_state)
        return -2;
    if(0 == len)
        return -3;

    /* 记录剩余大小 */
    unsigned int left_size = len;

    /* 计算需要的空闲簇数，注意文件末簇是否写完 */
    unsigned int to_alloc_num;
    to_alloc_num = (len > fileInfo->EndCluLeftSize)?\
        ( (len - fileInfo->EndCluLeftSize)/(PER_SECSIZE*g_dbr[0].secPerClus) + 1 ) : 1;
    if((len - fileInfo->EndCluLeftSize)%(PER_SECSIZE*g_dbr[0].secPerClus)) 
        to_alloc_num ++;
        
    /* 预生成文件簇链 */
    if(to_alloc_num > 1)
    {
        if(0 > YC_FAT_CreateFileCluChain(fileInfo,to_alloc_num-1))
        {
            /* 错误处理 */
            return -1;
        }
    }
    /* 灌数据 */
    if(to_alloc_num == 1){
        /* 锚定尾扇区 */
        char endsec = fileInfo->fl_sz%(PER_SECSIZE*g_dbr[0].secPerClus);
    }else{
        usr_write(d_buf,1,100);
    }

    struct list_head *pos,*tmp;
    /* 修改FAT表缝合簇链 */
    list_for_each_safe(pos, tmp, &fileInfo->WRCluChainList)
    {
        /* 找到压缩簇链所在FAT表扇区 */
        /* 先找出首簇所在FAT扇区 */
        unsigned int headsec = CLU_TO_FATSEC(((w_buffer_t *)pos)->w_s_clu);

        for(;;)
        {   
            /* 同扇区FAT缝合 */
            if(1){break;}
        }
        usr_write(cur_fat_sec,1,100);

        /* 备份FAT1至FAT2 */
        {;}
    }

    /* 删除写压缩缓冲簇链，释放内存 */
    list_for_each_safe(pos, tmp, &fileInfo->WRCluChainList)
    {
        list_del(pos);
        tFreeHeapforeach((void *)pos);
    }
    return 0;
}

/* 格式化磁盘 */
int YC_FAT_mkfs(unsigned DISK_ID)
{

}

/* 删除文件 */
int YC_FAT_Del_File()
{

}
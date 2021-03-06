/****************************************************************************
*  Copyright 2017 Gorgon Meducer (Email:embedded_zhuoran@hotmail.com)       *
*                                                                           *
*  Licensed under the Apache License, Version 2.0 (the "License");          *
*  you may not use this file except in compliance with the License.         *
*  You may obtain a copy of the License at                                  *
*                                                                           *
*     http://www.apache.org/licenses/LICENSE-2.0                            *
*                                                                           *
*  Unless required by applicable law or agreed to in writing, software      *
*  distributed under the License is distributed on an "AS IS" BASIS,        *
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. *
*  See the License for the specific language governing permissions and      *
*  limitations under the License.                                           *
*                                                                           *
****************************************************************************/

/*============================ INCLUDES ======================================*/
#include ".\app_cfg.h"

#if USE_SERVICE_STREAM_TO_BLOCK == ENABLED
#include "..\block_queue\block_queue.h"

#include <string.h>
/*============================ MACROS ========================================*/

#if USE_SERVICE_BLOCK_QUEUE != ENABLED
#error The Stream2Block service requires the Block Queue Service, please set USE_SERVICE_BLOCK_QUEUE to ENABLED in top app_cfg.h
#endif
#if USE_SERVICE_BLOCK != ENABLED
#error The Stream2Block service requires the Block Service, please set USE_SERVICE_BLOCK to ENABLED in top app_cfg.h
#endif


#ifndef __SB_ATOM_ACCESS
#   define __SB_ATOM_ACCESS(...)            SAFE_ATOM_CODE(__VA_ARGS__)
#endif

/*============================ MACROFIED FUNCTIONS ===========================*/
/*============================ TYPES =========================================*/


//! \note no thread safe queue is required
DEF_QUEUE_U8(StreamBufferQueue, uint_fast16_t, bool)
    
END_DEF_QUEUE_U8(StreamBufferQueue)

//! \brief stream buffer control block
//! @{
declare_class(stream_buffer_t)

typedef void stream_buffer_req_event_t(stream_buffer_t *ptThis);


typedef union {
    struct {
        uint8_t        IsAvailable         :1;
        uint8_t        IsOutput            :1;
        uint8_t        IsDataAvailable     :1;
        uint8_t        IsBlockBufferDrain  :1;
        uint8_t                            :4;
    };
    uint8_t tValue;
} stream_buffer_status_t;

def_class(stream_buffer_t, 
    which(   
        inherit(block_queue_t)                                                  //!< inherit from block_queue_t
        inherit(QUEUE(StreamBufferQueue))                                       //!< inherit from queue StreamBufferQueue
    )) 

    bool                                    bIsOutput;                          //!< direction
    bool                                    bIsQueueInitialised;                //!< Indicate whether the queue has been inialised or not
    block_pool_t                           *ptBlockPool;                        //!< a reference to outside block pool
    block_t                                *ptUsedByQueue;                      //!< buffer block used by queue
    block_t                                *ptUsedByOutside;                    //!< buffer block lent out  
    stream_buffer_req_event_t              *fnRequestSend;                      //!< callback for triggering the first output transaction
    stream_buffer_req_event_t              *fnRequestReceive; 
    
    uint8_t                                 chBlockReservedSize;
    stream_buffer_status_t                  tStatus;
end_def_class(stream_buffer_t,
    which(   
        inherit(block_queue_t)                                                  //!< inherit from block_queue_t
        inherit(QUEUE(StreamBufferQueue))                                       //!< inherit from queue StreamBufferQueue
    ))
//! @}

typedef struct {
    block_pool_t                           *ptPool;
    enum {
        INPUT_STREAM = 0,
        OUTPUT_STREAM
    } tDirection;
    
    stream_buffer_req_event_t              *fnRequestHandler;
    uint_fast8_t                            chBlockReservedSize; 
}stream_buffer_cfg_t;




def_interface(i_stream_buffer_t)

    bool        (*Init)         (stream_buffer_t *, stream_buffer_cfg_t *);
    stream_buffer_status_t
                (*Status)       (stream_buffer_t *);    
    struct {
        bool    (*ReadByte)     (stream_buffer_t *, uint8_t *);
        bool    (*WriteByte)    (stream_buffer_t *, uint8_t);
        bool    (*WriteBlock)   (stream_buffer_t *, block_t *ptBlock);
        bool    (*Flush)        (stream_buffer_t *ptObj);
    } Stream;
    
    struct {
        block_t *(*Exchange)     (stream_buffer_t *, block_t *);
        block_t *(*GetNext)      (stream_buffer_t *);
        void    (*Return)       (stream_buffer_t *, block_t *);
    } Block;

end_def_interface(i_stream_buffer_t)
    

/*============================ LOCAL VARIABLES ===============================*/
/*============================ PROTOTYPES ====================================*/
    
static bool stream_buffer_init(     stream_buffer_t *ptObj, 
                                    stream_buffer_cfg_t *ptCFG); 
static bool stream_read(            stream_buffer_t *ptObj, 
                                    uint8_t *pchData);
static bool stream_write(           stream_buffer_t *ptObj, 
                                    uint8_t chData);
static block_t *request_next_buffer_block(
                                    stream_buffer_t *ptObj, 
                                    block_t *ptOld);
static block_t *get_next_block(        stream_buffer_t *ptObj);
static void return_block(           stream_buffer_t *ptObj, block_t *ptOld);
static bool stream_flush(           stream_buffer_t *ptObj);
static bool stream_write_block(     stream_buffer_t *ptObj, block_t *ptBlock);
static void __append_block_to_output_list(
                                    stream_buffer_t *ptObj, block_t *ptBlock);
static stream_buffer_status_t get_status (stream_buffer_t *ptObj);

/*============================ IMPLEMENTATION ================================*/
/*============================ GLOBAL VARIABLES ==============================*/
    
const i_stream_buffer_t STREAM_BUFFER = {
        .Init =             &stream_buffer_init,
        .Status =           &get_status,
        .Stream = {
            .ReadByte =     &stream_read,
            .WriteByte =    &stream_write,
            .WriteBlock =   &stream_write_block,
            .Flush =        &stream_flush,
        },
        .Block = {
            .Exchange = &request_next_buffer_block,
            .GetNext =  &get_next_block,
            .Return =   &return_block,
        },
    };



static stream_buffer_status_t get_status (stream_buffer_t *ptObj)
{
    class_internal(ptObj, ptThis, stream_buffer_t);
    stream_buffer_status_t tStatus = {0};
    
    do {
        if (NULL == ptThis) {
            break;
        }
        
        tStatus = this.tStatus;
        
    } while(false);
    
    return tStatus;
}

static bool stream_buffer_init(stream_buffer_t *ptObj, stream_buffer_cfg_t *ptCFG)
{
    bool bResult = false;
    do {
        class_internal(ptObj, ptThis, stream_buffer_t);
        
        if (NULL == ptObj || NULL == ptCFG) {
            break;
        } else if (NULL == ptCFG->ptPool) {
            break;
        }
        
        __SB_ATOM_ACCESS(
            do {
                memset(ptObj, 0, sizeof(stream_buffer_t));
            
                this.ptBlockPool = ptCFG->ptPool;
                this.bIsOutput = (ptCFG->tDirection == OUTPUT_STREAM);
                this.bIsQueueInitialised = false;
                
                this.ptUsedByQueue = NULL;
                this.ptUsedByOutside = NULL;
                if (this.bIsOutput) {
                    this.fnRequestSend = ptCFG->fnRequestHandler;
                } else {
                    this.fnRequestReceive = ptCFG->fnRequestHandler;
                }
                
                
                this.chBlockReservedSize = ptCFG->chBlockReservedSize;
                
                BLOCK_QUEUE.Init(ref_obj_as(this, block_queue_t));
                
                this.tStatus.IsAvailable = true;
                this.tStatus.IsOutput = this.bIsOutput;
                this.tStatus.IsBlockBufferDrain = true;
                
                bResult = true;
            } while(false);
        )
    } while(false);
    
    return bResult;
}


static block_t *__get_new_block(stream_buffer_t *ptObj)
{
    block_t *ptItem = NULL;
    class_internal(ptObj, ptThis, stream_buffer_t);
    
    //! get a new block
    ptItem = BLOCK.Heap.New( this.ptBlockPool);
    
    if (NULL != ptItem) {
        //reserve block size
        uint32_t wSize = BLOCK.Size.Get(ptItem);
        if (this.chBlockReservedSize < wSize) {
            BLOCK.Size.Set(ptItem, wSize - this.chBlockReservedSize);
        }
        
    }
    
    return ptItem;
}

static block_t *get_next_block(stream_buffer_t *ptObj)
{
    block_t *ptItem = NULL;
    class_internal(ptObj, ptThis, stream_buffer_t);
    
    do {
        
        if (NULL == ptThis) {
            break;
        }

        if (this.bIsOutput) {
            __SB_ATOM_ACCESS(
                //! find the next block from the list
                ptItem = BLOCK_QUEUE.Dequeue( ref_obj_as(this, block_queue_t));
                if (0 == BLOCK_QUEUE.Count(ref_obj_as(this, block_queue_t))) {
                    this.tStatus.IsBlockBufferDrain = true;
                }
            )
        } else {
            ptItem = __get_new_block(ptObj);
        }
        
        this.ptUsedByOutside = ptItem;
        
    } while(false);

    return ptItem;
}

static void return_block(stream_buffer_t *ptObj, block_t *ptItem)
{
    class_internal(ptObj, ptThis, stream_buffer_t);
    do {
        if (NULL == ptThis || NULL == ptItem) {
            break;
        }
       
            
        if (this.bIsOutput) {
            
            //! reset block size
            BLOCK.Size.Reset(ptItem);
            //! stream is used for output
            BLOCK.Heap.Free(this.ptBlockPool, ptItem);
            
        } else {
        
            __SB_ATOM_ACCESS(
                //! stream is used for input
                //! add block to the list
                BLOCK_QUEUE.Enqueue( ref_obj_as(this, block_queue_t), ptItem);
                this.tStatus.IsDataAvailable = true;
                this.tStatus.IsBlockBufferDrain = false;
            )
        }
    } while(false);

}

static block_t *request_next_buffer_block(stream_buffer_t *ptObj, block_t *ptOld)
{
    block_t *ptItem = NULL;
    class_internal(ptObj, ptThis, stream_buffer_t);
    
    do {
        if (NULL == ptThis) {
            break;
        }
        
        if (this.bIsOutput) {
            
            if (NULL != ptOld) {
                //! reset block size
                BLOCK.Size.Reset(ptOld);
                //! stream is used for output
                BLOCK.Heap.Free(this.ptBlockPool, ptOld);
            }
            
            __SB_ATOM_ACCESS(
                //! find the next block from the list
                ptItem = BLOCK_QUEUE.Dequeue( ref_obj_as(this, block_queue_t));
            
                if (0 == BLOCK_QUEUE.Count(ref_obj_as(this, block_queue_t))) {
                    this.tStatus.IsBlockBufferDrain = true;
                }
            )
        } else {
            if (NULL != ptOld) {
                __SB_ATOM_ACCESS(
                    //! stream is used for input
                    //! add block to the list
                    BLOCK_QUEUE.Enqueue( ref_obj_as(this, block_queue_t), ptOld);
                    this.tStatus.IsDataAvailable = true;
                    this.tStatus.IsBlockBufferDrain = false;
                )
            }
            
            ptItem = __get_new_block(ptObj);
        }
        
        this.ptUsedByOutside = ptItem;
        
    } while(false);

    return ptItem;
}

static bool queue_init(stream_buffer_t *ptObj, bool bIsStreamForRead)
{
    class_internal(ptObj, ptThis, stream_buffer_t);
    block_t *ptBlock;
    
    if (bIsStreamForRead) {
        
        if (NULL != this.ptUsedByQueue) {
            BLOCK.Size.Reset(this.ptUsedByQueue);

            BLOCK.Heap.Free( this.ptBlockPool, this.ptUsedByQueue);
        }
        
        __SB_ATOM_ACCESS (
            //! fetch a block from list, and initialise it as a full queue
            ptBlock = BLOCK_QUEUE.Dequeue( ref_obj_as(this, block_queue_t) );
            if (NULL == ptBlock) {
                this.tStatus.IsDataAvailable = false;
            }
            
            if (0 == BLOCK_QUEUE.Count(ref_obj_as(this, block_queue_t))) {
                this.tStatus.IsBlockBufferDrain = true;
            }
        )
        
        if ( NULL == this.ptUsedByOutside) {
            if (NULL != this.fnRequestReceive) {
                (*this.fnRequestReceive)(ptObj);
            }
        }
        
        
    } else {
        
        //! add buffer to block list ( which is used as output list)
        do {
            if (NULL == this.ptUsedByQueue) {
                break;
            }
            
            //! use queue count as actual size
            BLOCK.Size.Set( this.ptUsedByQueue, 
                            GET_QUEUE_COUNT( StreamBufferQueue, 
                                             REF_OBJ_AS(this, 
                                                QUEUE(StreamBufferQueue))));
            
            __append_block_to_output_list(ptObj, this.ptUsedByQueue);

        } while(false);

        ptBlock = __get_new_block(ptObj);
        
    }

    if (NULL == ptBlock) {
        //! queue is empty
        return false;
    }
    
    this.ptUsedByQueue = (block_t *)ptBlock;
    
    QUEUE_INIT_EX(  StreamBufferQueue,                                          //!< queue name
                    REF_OBJ_AS(this, QUEUE(StreamBufferQueue)),                 //!< queue obj
                    (uint8_t *)(BLOCK.Buffer.Get(ptBlock)),                     //!< buffer
                    BLOCK.Size.Get(ptBlock),                                    //!< buffer size
                    bIsStreamForRead);                                          //!< intialize method (initialise as full or initialise as empty)
    
    
    this.bIsQueueInitialised = true;
    
    return true;
}

static bool stream_flush(stream_buffer_t *ptObj)
{
    class_internal(ptObj, ptThis, stream_buffer_t);
    bool bResult = false;
    
    do {
        if (NULL == ptThis) {
            break ;
        }
        if (this.bIsOutput) {
            if (0 != GET_QUEUE_COUNT(StreamBufferQueue, 
                                REF_OBJ_AS(this, QUEUE(StreamBufferQueue)))) {
                
                if (!queue_init(ptObj, false)) {
                    //! queue is empty
                    this.bIsQueueInitialised = false;
                }
            }
            
            bResult = ( NULL == TYPE_CONVERT( &(this.ptUsedByOutside), block_t * volatile));
        } else {
            bResult = true;
        }
    } while(false);
    
    return bResult;
}


static bool stream_read(stream_buffer_t *ptObj, uint8_t *pchData)
{
    do {
        class_internal(ptObj, ptThis, stream_buffer_t);
        if (NULL == ptThis || NULL == pchData) {
            break;
        } else if (this.bIsOutput) {
            break;
        }
        
        do {
            if (!this.bIsQueueInitialised) {
                if (!queue_init(ptObj, true)) {
                    //! queue is empty
                    return false;
                }
            }
            
            if (!DEQUEUE(   StreamBufferQueue, 
                            REF_OBJ_AS(this, QUEUE(StreamBufferQueue)), pchData)) {
                this.bIsQueueInitialised = false;
            } else {
                return true;
            }
        } while(true);
        
    } while(false);
    
    return false;
}



static bool stream_write(stream_buffer_t *ptObj, uint8_t chData)
{
    do {
        class_internal(ptObj, ptThis, stream_buffer_t);
        if (NULL == ptObj) {
            break;
        } else if (!this.bIsOutput) {
            break;
        }
        
        do {
            if (!this.bIsQueueInitialised) {
                if (!queue_init(ptObj, false)) {
                    //! queue is empty
                    return false;
                }
            }
            
            if (!ENQUEUE( StreamBufferQueue, REF_OBJ_AS(this, QUEUE(StreamBufferQueue)), chData)) {
                this.bIsQueueInitialised = false;
            } else {
                return true;
            }
        } while(true);
        
    } while(false);
    
    return false;
}

static void __append_block_to_output_list(  stream_buffer_t *ptObj, 
                                            block_t *ptBlock)
{
    class_internal(ptObj, ptThis, stream_buffer_t);

    __SB_ATOM_ACCESS(
        BLOCK_QUEUE.Enqueue( ref_obj_as(this, block_queue_t), ptBlock);
        this.tStatus.IsBlockBufferDrain = false;
    )
    
    if (NULL == this.ptUsedByOutside ) {
        //! this is no transaction, we need to trigger one
        if (NULL != this.fnRequestSend) {
            (*this.fnRequestSend)(ptObj);
        }
    }
}

static bool stream_write_block(stream_buffer_t *ptObj, block_t *ptBlock)
{
    do {
        class_internal(ptObj, ptThis, stream_buffer_t);
        if (NULL == ptThis || NULL == ptBlock) {
            break;
        } else if (!this.bIsOutput) {
            break;
        }
        
        //! write current block used by queue to output list
        do {
            if (0 == GET_QUEUE_COUNT(StreamBufferQueue, 
                            REF_OBJ_AS(this, QUEUE(StreamBufferQueue)))) {
                break;
            }
            if (!queue_init(ptObj, false)) {
                //! queue is empty
                this.bIsQueueInitialised = false;
            }
        } while(true);
        
        __append_block_to_output_list(ptObj, ptBlock);
        return true;
        
    } while(false);
    
    return false;
}




#endif
/* EOF */
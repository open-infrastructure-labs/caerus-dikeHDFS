#ifndef DIKE_BINARY_COLUMN_WRITER_HPP
#define DIKE_BINARY_COLUMN_WRITER_HPP

#include <iostream>
#include <chrono> 
#include <ctime>
#include <string>
#include <sstream> 
#include <iomanip>
#include <thread>
#include <queue>
#include <mutex>
#include <cassert>
#include <semaphore.h>
#include <unistd.h>

#include "DikeUtil.hpp"
#include "DikeIO.hpp"
#include "DikeBuffer.hpp"
#include "DikeAsyncWriter.hpp"

enum {
    BINARY_COLUMN_BATCH_SIZE = 4096,
};

class DikeBinaryColumn {
    public:
    int data_type;

    uint8_t * start_pos = 0;
    uint8_t * pos = 0;
    uint8_t * end_pos = 0;

    uint8_t * start_idx = 0;
    uint8_t * idx_pos = 0;
    
    DikeBinaryColumn(int data_type) {
        this->data_type = data_type;
        switch(data_type){
            case SQLITE_INTEGER:
            case SQLITE_FLOAT: // We transfering 8 bytes in Big Endian
                start_pos = new uint8_t [BINARY_COLUMN_BATCH_SIZE * sizeof(int64_t)];
                pos = start_pos;
                end_pos = pos + BINARY_COLUMN_BATCH_SIZE * sizeof(int64_t);
            break;
            
            case SQLITE3_TEXT:
                start_pos = new uint8_t [BINARY_COLUMN_BATCH_SIZE * 64]; // to fit in 256K
                pos = start_pos;
                end_pos = pos + BINARY_COLUMN_BATCH_SIZE * 64;
                start_idx = new uint8_t [BINARY_COLUMN_BATCH_SIZE];
                idx_pos = start_idx;
            break;
        }
    }

    ~DikeBinaryColumn() {
        delete start_pos;
        if(start_idx) {
            delete start_idx;
        }
    }
};

class DikeBinaryColumnWriter : public DikeAsyncWriter {
    public:
    int64_t row_count = 0;
    int64_t batch_count = 0;
    int64_t data_count = 0; // Number of columns
    int64_t * data_types = NULL;
    DikeBinaryColumn ** columns = NULL;

    DikeBinaryColumnWriter(DikeIO * output) : DikeAsyncWriter(output) { }

    virtual ~DikeBinaryColumnWriter() override {
        if(data_types) {
            delete data_types;
        }
        if( columns ) {
            for(int i = 0; i < data_count; i++) {
                delete columns[i];
            }
            delete columns;
        }
    }

    virtual void close() override {
        flush();
        DikeAsyncWriter::close();
    }

    void InitializeSchema(sqlite3_stmt *sqlRes) {
        data_count = sqlite3_column_count(sqlRes);
        data_types = new int64_t [data_count];
        columns = new DikeBinaryColumn * [data_count];

        for(int i = 0; i < data_count; i++) {
            data_types[i] = sqlite3_column_type(sqlRes, i);
            columns[i] = new DikeBinaryColumn(data_types[i]);
        }

        // This is our first write, so buffer should have enough space
        int64_t be_value = htobe64(data_count);
        buffer->write(&be_value, sizeof(int64_t));
        for(int i = 0; i < data_count; i++) {
            be_value = htobe64(data_types[i]);
            //std::cout << i << " : " << data_types[i] << std::endl;
            buffer->write(&be_value, sizeof(int64_t));
        }
    }

    virtual int write(sqlite3_stmt *sqlRes) override {
        if (data_count == 0) {
            InitializeSchema(sqlRes);
        }

        if(!isRunning){
            return 0;
        }        
  
        int64_t be_value;
        for(int i = 0; i < data_count; i++) {
            //int data_type = sqlite3_column_type(sqlRes, i);
            switch(data_types[i]) {
                case SQLITE_INTEGER:
                {
                    int64_t int64_value = sqlite3_column_int64(sqlRes, i);                    
                    *(int64_t*)columns[i]->pos = htobe64(int64_value);
                    columns[i]->pos += sizeof(int64_t);
                }
                break;
                case SQLITE_FLOAT:
                {
                    double double_value = sqlite3_column_double(sqlRes, i);
                    *(int64_t*)columns[i]->pos = htobe64(*(int64_t*)&double_value);
                    columns[i]->pos += sizeof(int64_t);

                }
                break;
                case SQLITE3_TEXT:
                {
                    uint32_t column_bytes = sqlite3_column_bytes(sqlRes, i);
                    const uint8_t* column_text = sqlite3_column_text(sqlRes, i);
                    memcpy(columns[i]->pos, column_text, column_bytes);
                    columns[i]->pos += column_bytes;
                    *columns[i]->idx_pos = column_bytes;
                    columns[i]->idx_pos++;
                }
                break;                        
            }
        }

        row_count++;
        batch_count++;
        if(batch_count >= BINARY_COLUMN_BATCH_SIZE){
            flush();
            batch_count = 0;
        }
        return 1;
    }

    void flush() {
        for(int i = 0; i < data_count; i++) {
            int64_t byte_count;
            int64_t be_value;
            int rc;

            if(data_types[i] == SQLITE3_TEXT){
                byte_count = columns[i]->idx_pos - columns[i]->start_idx;
                //std::cout << i << " : byte_count : " << byte_count << std::endl;
                be_value =  htobe64(byte_count);
                rc = buffer->write(&be_value, sizeof(int64_t));
                if (rc == 0){
                    buffer = getBuffer();
                    rc = buffer->write(&be_value, sizeof(int64_t));
                }
                rc = buffer->write(columns[i]->start_idx, byte_count);
                if (rc == 0){
                    buffer = getBuffer();
                    rc = buffer->write(columns[i]->start_idx, byte_count);
                }

                columns[i]->idx_pos = columns[i]->start_idx;
            }

            byte_count = columns[i]->pos - columns[i]->start_pos;
            be_value =  htobe64(byte_count);
            rc = buffer->write(&be_value, sizeof(int64_t));
            if (rc == 0){
                buffer = getBuffer();
                rc = buffer->write(&be_value, sizeof(int64_t));
            }
            rc = buffer->write(columns[i]->start_pos, byte_count);
            if (rc == 0){
                buffer = getBuffer();
                rc = buffer->write(columns[i]->start_pos, byte_count);
            }

            columns[i]->pos = columns[i]->start_pos;
        }
    }
};

#endif /* DIKE_BINARY_COLUMN_WRITER_HPP */
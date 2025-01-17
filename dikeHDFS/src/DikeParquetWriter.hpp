#ifndef DIKE_PARQUET_WRITER_HPP
#define DIKE_PARQUET_WRITER_HPP

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

#include <parquet/api/writer.h>

#include "DikeUtil.hpp"
#include "DikeIO.hpp"
#include "DikeBuffer.hpp"
#include "DikeAsyncWriter.hpp"

class DikeOutputStream : public ::arrow::io::OutputStream /* ::arrow::io::FileOutputStream */ {    
    public:
    int64_t pos = 0;
    DikeAsyncWriter * dikeAsyncWriter = NULL;
    bool is_closed = false;

    DikeOutputStream(DikeAsyncWriter * dikeAsyncWriter)  {
        this->dikeAsyncWriter = dikeAsyncWriter;
    }

    virtual ~DikeOutputStream() {}

    virtual arrow::Status Write(const void* data, int64_t nbytes) override {  
        //std::cout << "DikeOutputStream::Write " << nbytes << std::endl;
        dikeAsyncWriter->output->write((const char*)data, nbytes);
        pos += nbytes;
        return arrow::Status::OK(); 
    }

    virtual arrow::Status Write(const std::shared_ptr<arrow::Buffer>& data) override {
        return this->Write((const void*) data->data(), (int64_t) data->size());
    }
    
    virtual arrow::Status Flush() override {
        //std::cout << "DikeOutputStream::Flush " << std::endl;        
        return arrow::Status::OK();
    }

    virtual arrow::Status Close() override {
        //std::cout << "DikeOutputStream::Close at pos " << pos << std::endl;        
        is_closed = true;
        return arrow::Status::OK();
    }

    virtual bool closed() const override {        
        return is_closed;        
    }
    
    virtual arrow::Result<int64_t> Tell() const override {
        //std::cout << __func__ << " : " << pos << std::endl;
        return pos;        
    }
};

class DikeColumnWriter {
    public:
    parquet::ColumnWriter * columnWriter = NULL;
    int dataType = 0;    
    int pos = 0;
    int64_t * int64_values = NULL;
    double * double_values = NULL;
    parquet::ByteArray * ba_values = NULL;
    uint8_t * ba_buffer = NULL;
    uint8_t * ba_buffer_pos = NULL;
    uint8_t * ba_buffer_end = NULL;
    enum {
        DIKE_COLUMN_WRITER_BATCH_SIZE = 1 << 20,
    };
    DikeColumnWriter(parquet::ColumnWriter * columnWriter, int dataType) {
        this->columnWriter = columnWriter;
        this->dataType = dataType;
        switch(dataType){
            case SQLITE_INTEGER:
            int64_values = new int64_t [DIKE_COLUMN_WRITER_BATCH_SIZE];
            break;
            case SQLITE_FLOAT:
            double_values = new double [DIKE_COLUMN_WRITER_BATCH_SIZE];
            break;
           case SQLITE3_TEXT:
            ba_values = new parquet::ByteArray [DIKE_COLUMN_WRITER_BATCH_SIZE];
            ba_buffer = new uint8_t [DIKE_COLUMN_WRITER_BATCH_SIZE * sizeof(int64_t)];
            ba_buffer_pos = ba_buffer;
            ba_buffer_end = ba_buffer + DIKE_COLUMN_WRITER_BATCH_SIZE * sizeof(int64_t);
            break;
        }
    }

    ~DikeColumnWriter() {
        if(int64_values) {
            delete [] int64_values;
        }
        if(double_values) {
            delete [] double_values;
        }
        if(ba_values) {
            delete [] ba_values;
        }        
        if(ba_buffer) {
            delete [] ba_buffer;
        }        

    }
    void Close() {
        if(pos == 0){
            return;
        }
        switch(dataType){
            case SQLITE_INTEGER:
                static_cast<parquet::Int64Writer*>(columnWriter)->WriteBatch(pos, nullptr, nullptr, int64_values);
            break;
            case SQLITE_FLOAT:
                static_cast<parquet::DoubleWriter*>(columnWriter)->WriteBatch(pos, nullptr, nullptr, double_values);
            break;
            case SQLITE3_TEXT:
                static_cast<parquet::ByteArrayWriter*>(columnWriter)->WriteBatch(pos, nullptr, nullptr, ba_values);
                ba_buffer_pos = ba_buffer;
            break;
        }
        pos = 0;
    }

    void Write(int64_t value) {
        if (pos >= DIKE_COLUMN_WRITER_BATCH_SIZE) {
            static_cast<parquet::Int64Writer*>(columnWriter)->WriteBatch(pos, nullptr, nullptr, int64_values);
            pos = 0;
        }
        int64_values[pos] = value;
        pos++;
    }

    void Write(double value) {
        if (pos >= DIKE_COLUMN_WRITER_BATCH_SIZE) {
            static_cast<parquet::DoubleWriter*>(columnWriter)->WriteBatch(pos, nullptr, nullptr, double_values);
            pos = 0;
        }
        double_values[pos] = value;
        pos++;
    }

    void Write(const uint8_t* ptr,  uint32_t len ) {
        if (pos >= DIKE_COLUMN_WRITER_BATCH_SIZE || (ba_buffer_pos + len >= ba_buffer_end)) {
            static_cast<parquet::ByteArrayWriter*>(columnWriter)->WriteBatch(pos, nullptr, nullptr, ba_values);
            pos = 0;
            ba_buffer_pos = ba_buffer;
        }
        ba_values[pos].ptr = ba_buffer_pos;
        ba_values[pos].len = len;
        memcpy(ba_buffer_pos, ptr, len);
        ba_buffer_pos += len;
        pos++;
    }        
};

class DikeParquetWriter : public DikeAsyncWriter {
    public:
    std::shared_ptr<parquet::ParquetFileWriter> file_writer;
    std::shared_ptr<DikeOutputStream> out_file;
    parquet::RowGroupWriter* rg_writer = NULL;
    DikeColumnWriter ** cl_writers = NULL;
    bool is_closed = false;
    int64_t row_count = 0;
    int data_count = 0; // Number of columns

    DikeParquetWriter(DikeIO * output) : DikeAsyncWriter(output) { }

    virtual ~DikeParquetWriter() override {
        if (!is_closed) {
            close();
        }
        if (cl_writers != NULL) {
            for(int i = 0; i < data_count; i++) {            
               delete cl_writers[i];
            }        
 
            delete cl_writers;
        }
    }

    virtual void close() override {
        for(int i = 0; i < data_count; i++) {            
            cl_writers[i]->Close();
        }        
        rg_writer->Close();
        file_writer->Close();
        out_file->Close();
        is_closed = true;
        DikeAsyncWriter::close();
    }

    void InitWriters(sqlite3_stmt *sqlRes) {
        using namespace parquet;
        schema::NodeVector fields;

        data_count =  sqlite3_column_count(sqlRes);        

        for(int i = 0; i < data_count; i++) {
            int data_type = sqlite3_column_type(sqlRes, i);
            const char * column_name = sqlite3_column_name(sqlRes, i);            
            switch(data_type) {
                case SQLITE_INTEGER:
                    //std::cout << "DikeParquetWriter::InitWriter  " << i << " " << column_name << " SQLITE_INTEGER" << std::endl;
                    fields.push_back(schema::PrimitiveNode::Make(column_name, Repetition::REQUIRED, Type::INT64));
                break;
                case SQLITE_FLOAT:
                    //std::cout << "DikeParquetWriter::InitWriter  " << i << " " << column_name << " SQLITE_FLOAT" << std::endl;
                    fields.push_back(schema::PrimitiveNode::Make(column_name, Repetition::REQUIRED, Type::DOUBLE));
                break;
                case SQLITE3_TEXT:
                    //std::cout << "DikeParquetWriter::InitWriter  " << i << " " << column_name << " SQLITE3_TEXT" << std::endl;
                    fields.push_back(schema::PrimitiveNode::Make(column_name, Repetition::REQUIRED, Type::BYTE_ARRAY));
                break;                        
            }
        }
        // Create Schema
        std::shared_ptr<schema::GroupNode> schema = 
            std::static_pointer_cast<schema::GroupNode>(schema::GroupNode::Make("schema", Repetition::REQUIRED, fields));

        // Add writer properties
        parquet::WriterProperties::Builder builder;
        builder.compression(parquet::Compression::SNAPPY);

        //builder.data_pagesize(64 * 1024);
        builder.disable_dictionary();
        builder.disable_statistics();
        builder.max_row_group_length(1<<30);

        std::shared_ptr<parquet::WriterProperties> writerProperties = builder.build();

        out_file = std::shared_ptr<DikeOutputStream>(new DikeOutputStream(this));
        file_writer = parquet::ParquetFileWriter::Open(out_file, schema, writerProperties);
        
        rg_writer = file_writer->AppendBufferedRowGroup();

        cl_writers = new DikeColumnWriter *[data_count];
        for(int i = 0; i < data_count; i++) {
            int data_type = sqlite3_column_type(sqlRes, i);
            cl_writers[i] = new DikeColumnWriter(rg_writer->column(i), data_type);
        }
    }

    virtual int write(void * res) override {
        sqlite3_stmt *sqlRes  = (sqlite3_stmt *)res;
        if (cl_writers == NULL) {
            InitWriters(sqlRes);
        }        
        
        for(int i = 0; i < data_count; i++) {
            //int data_type = sqlite3_column_type(sqlRes, i);
            switch(cl_writers[i]->dataType) {
                case SQLITE_INTEGER:
                {
                    //std::cout << "DikeParquetWriter::write  " << i << " SQLITE_INTEGER " << std::endl;
                    int64_t int64_value = sqlite3_column_int64(sqlRes, i);
                    //static_cast<parquet::Int64Writer*>(cl_writers[i])->WriteBatch(1, nullptr, nullptr, &int64_value);
                    cl_writers[i]->Write(int64_value);
                }
                break;
                case SQLITE_FLOAT:
                {
                    //std::cout << "DikeParquetWriter::write  " << i << " SQLITE_FLOAT " << std::endl;
                    double double_value = sqlite3_column_double(sqlRes, i);
                    //static_cast<parquet::DoubleWriter*>(cl_writers[i])->WriteBatch(1, nullptr, nullptr, &double_value);
                    cl_writers[i]->Write(double_value);
                }
                break;
                case SQLITE3_TEXT:
                {
                    uint32_t column_bytes = sqlite3_column_bytes(sqlRes, i);
                    const uint8_t* column_text = sqlite3_column_text(sqlRes, i);
                    //parquet::ByteArray ba_value(column_bytes, column_text);
                    //std::cout << "DikeParquetWriter::write  " << i << " SQLITE3_TEXT " << std::endl;
                    //static_cast<parquet::ByteArrayWriter*>(cl_writers[i])->WriteBatch(1, nullptr, nullptr, &ba_value);
                    cl_writers[i]->Write(column_text, column_bytes);
                }
                break;                        
            }
        }

        row_count++;
        return 1;
    }
};

#endif /* DIKE_PARQUET_WRITER_HPP */
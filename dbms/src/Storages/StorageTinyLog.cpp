#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include <map>

#include <Poco/Util/XMLConfiguration.h>

#include <Common/escapeForFileName.h>
#include <Common/Exception.h>
#include <Common/typeid_cast.h>

#include <Compression/CompressionFactory.h>
#include <Compression/CompressedReadBuffer.h>
#include <Compression/CompressedWriteBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>

#include <DataTypes/NestedUtils.h>

#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/IBlockOutputStream.h>

#include <Columns/ColumnArray.h>

#include <Interpreters/Context.h>

#include <Storages/StorageTinyLog.h>
#include <Storages/StorageFactory.h>
#include <Storages/CheckResults.h>

#define DBMS_STORAGE_LOG_DATA_FILE_EXTENSION ".bin"


namespace DB
{

namespace ErrorCodes
{
    extern const int EMPTY_LIST_OF_COLUMNS_PASSED;
    extern const int CANNOT_CREATE_DIRECTORY;
    extern const int CANNOT_READ_ALL_DATA;
    extern const int DUPLICATE_COLUMN;
    extern const int LOGICAL_ERROR;
    extern const int INCORRECT_FILE_NAME;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}


class TinyLogBlockInputStream final : public IBlockInputStream
{
public:
    TinyLogBlockInputStream(size_t block_size_, const NamesAndTypesList & columns_, StorageTinyLog & storage_, size_t max_read_buffer_size_)
        : block_size(block_size_), columns(columns_),
        storage(storage_), lock(storage_.rwlock),
        max_read_buffer_size(max_read_buffer_size_) {}

    String getName() const override { return "TinyLog"; }

    Block getHeader() const override
    {
        Block res;

        for (const auto & name_type : columns)
            res.insert({ name_type.type->createColumn(), name_type.type, name_type.name });

        return Nested::flatten(res);
    }

protected:
    Block readImpl() override;
private:
    size_t block_size;
    NamesAndTypesList columns;
    StorageTinyLog & storage;
    std::shared_lock<std::shared_mutex> lock;
    bool finished = false;
    size_t max_read_buffer_size;

    struct Stream
    {
        Stream(const DiskPtr & disk, const String & data_path, size_t max_read_buffer_size_)
            : plain(disk->readFile(data_path, std::min(max_read_buffer_size_, disk->getFileSize(data_path)))),
            compressed(*plain)
        {
        }

        std::unique_ptr<ReadBuffer> plain;
        CompressedReadBuffer compressed;
    };

    using FileStreams = std::map<String, std::unique_ptr<Stream>>;
    FileStreams streams;

    using DeserializeState = IDataType::DeserializeBinaryBulkStatePtr;
    using DeserializeStates = std::map<String, DeserializeState>;
    DeserializeStates deserialize_states;

    void readData(const String & name, const IDataType & type, IColumn & column, UInt64 limit);
};


class TinyLogBlockOutputStream final : public IBlockOutputStream
{
public:
    explicit TinyLogBlockOutputStream(StorageTinyLog & storage_)
        : storage(storage_), lock(storage_.rwlock)
    {
    }

    ~TinyLogBlockOutputStream() override
    {
        try
        {
            writeSuffix();
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
        }
    }

    Block getHeader() const override { return storage.getSampleBlock(); }

    void write(const Block & block) override;
    void writeSuffix() override;

private:
    StorageTinyLog & storage;
    std::unique_lock<std::shared_mutex> lock;
    bool done = false;

    struct Stream
    {
        Stream(const DiskPtr & disk, const String & data_path, CompressionCodecPtr codec, size_t max_compress_block_size) :
            plain(disk->writeFile(data_path, max_compress_block_size, WriteMode::Append)),
            compressed(*plain, std::move(codec), max_compress_block_size)
        {
        }

        std::unique_ptr<WriteBuffer> plain;
        CompressedWriteBuffer compressed;

        void finalize()
        {
            compressed.next();
            plain->finalize();
        }
    };

    using FileStreams = std::map<String, std::unique_ptr<Stream>>;
    FileStreams streams;

    using SerializeState = IDataType::SerializeBinaryBulkStatePtr;
    using SerializeStates = std::map<String, SerializeState>;
    SerializeStates serialize_states;

    using WrittenStreams = std::set<String>;

    IDataType::OutputStreamGetter createStreamGetter(const String & name, WrittenStreams & written_streams);
    void writeData(const String & name, const IDataType & type, const IColumn & column, WrittenStreams & written_streams);
};


Block TinyLogBlockInputStream::readImpl()
{
    Block res;

    if (finished || (!streams.empty() && streams.begin()->second->compressed.eof()))
    {
        /** Close the files (before destroying the object).
          * When many sources are created, but simultaneously reading only a few of them,
          * buffers don't waste memory.
          */
        finished = true;
        streams.clear();
        return res;
    }

    /// if there are no files in the folder, it means that the table is empty
    if (storage.disk->isDirectoryEmpty(storage.table_path))
        return res;

    for (const auto & name_type : columns)
    {
        MutableColumnPtr column = name_type.type->createColumn();

        try
        {
            readData(name_type.name, *name_type.type, *column, block_size);
        }
        catch (Exception & e)
        {
            e.addMessage("while reading column " + name_type.name + " at " + fullPath(storage.disk, storage.table_path));
            throw;
        }

        if (column->size())
            res.insert(ColumnWithTypeAndName(std::move(column), name_type.type, name_type.name));
    }

    if (!res || streams.begin()->second->compressed.eof())
    {
        finished = true;
        streams.clear();
    }

    return Nested::flatten(res);
}


void TinyLogBlockInputStream::readData(const String & name, const IDataType & type, IColumn & column, UInt64 limit)
{
    IDataType::DeserializeBinaryBulkSettings settings; /// TODO Use avg_value_size_hint.
    settings.getter = [&] (const IDataType::SubstreamPath & path) -> ReadBuffer *
    {
        String stream_name = IDataType::getFileNameForStream(name, path);

        if (!streams.count(stream_name))
            streams[stream_name] = std::make_unique<Stream>(storage.disk, storage.files[stream_name].data_file_path, max_read_buffer_size);

        return &streams[stream_name]->compressed;
    };

    if (deserialize_states.count(name) == 0)
         type.deserializeBinaryBulkStatePrefix(settings, deserialize_states[name]);

    type.deserializeBinaryBulkWithMultipleStreams(column, limit, settings, deserialize_states[name]);
}


IDataType::OutputStreamGetter TinyLogBlockOutputStream::createStreamGetter(const String & name,
                                                                           WrittenStreams & written_streams)
{
    return [&] (const IDataType::SubstreamPath & path) -> WriteBuffer *
    {
        String stream_name = IDataType::getFileNameForStream(name, path);

        if (!written_streams.insert(stream_name).second)
            return nullptr;

        const auto & columns = storage.getColumns();
        if (!streams.count(stream_name))
            streams[stream_name] = std::make_unique<Stream>(storage.disk,
                                                            storage.files[stream_name].data_file_path,
                                                            columns.getCodecOrDefault(name),
                                                            storage.max_compress_block_size);

        return &streams[stream_name]->compressed;
    };
}


void TinyLogBlockOutputStream::writeData(const String & name, const IDataType & type, const IColumn & column, WrittenStreams & written_streams)
{
    IDataType::SerializeBinaryBulkSettings settings;
    settings.getter = createStreamGetter(name, written_streams);

    if (serialize_states.count(name) == 0)
        type.serializeBinaryBulkStatePrefix(settings, serialize_states[name]);

    type.serializeBinaryBulkWithMultipleStreams(column, 0, 0, settings, serialize_states[name]);
}


void TinyLogBlockOutputStream::writeSuffix()
{
    if (done)
        return;
    done = true;

    /// If nothing was written - leave the table in initial state.
    if (streams.empty())
        return;

    WrittenStreams written_streams;
    IDataType::SerializeBinaryBulkSettings settings;
    for (const auto & column : getHeader())
    {
        auto it = serialize_states.find(column.name);
        if (it != serialize_states.end())
        {
            settings.getter = createStreamGetter(column.name, written_streams);
            column.type->serializeBinaryBulkStateSuffix(settings, it->second);
        }
    }

    /// Finish write.
    for (auto & stream : streams)
        stream.second->finalize();

    Strings column_files;
    for (auto & pair : streams)
        column_files.push_back(storage.files[pair.first].data_file_path);

    storage.file_checker.update(column_files.begin(), column_files.end());

    streams.clear();
}


void TinyLogBlockOutputStream::write(const Block & block)
{
    storage.check(block, true);

    /// The set of written offset columns so that you do not write shared columns for nested structures multiple times
    WrittenStreams written_streams;

    for (size_t i = 0; i < block.columns(); ++i)
    {
        const ColumnWithTypeAndName & column = block.safeGetByPosition(i);
        writeData(column.name, *column.type, *column.column, written_streams);
    }
}


StorageTinyLog::StorageTinyLog(
    DiskPtr disk_,
    const String & relative_path_,
    const String & database_name_,
    const String & table_name_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    bool attach,
    size_t max_compress_block_size_)
    : disk(std::move(disk_)), table_path(relative_path_), database_name(database_name_), table_name(table_name_),
    max_compress_block_size(max_compress_block_size_),
    file_checker(disk, table_path + "sizes.json"),
    log(&Logger::get("StorageTinyLog"))
{
    setColumns(columns_);
    setConstraints(constraints_);

    if (relative_path_.empty())
        throw Exception("Storage " + getName() + " requires data path", ErrorCodes::INCORRECT_FILE_NAME);

    if (!attach)
    {
        /// create directories if they do not exist
        disk->createDirectories(table_path);
    }

    for (const auto & col : getColumns().getAllPhysical())
        addFiles(col.name, *col.type);
}


void StorageTinyLog::addFiles(const String & column_name, const IDataType & type)
{
    if (files.end() != files.find(column_name))
        throw Exception("Duplicate column with name " + column_name + " in constructor of StorageTinyLog.",
            ErrorCodes::DUPLICATE_COLUMN);

    IDataType::StreamCallback stream_callback = [&] (const IDataType::SubstreamPath & substream_path)
    {
        String stream_name = IDataType::getFileNameForStream(column_name, substream_path);
        if (!files.count(stream_name))
        {
            ColumnData column_data;
            files.insert(std::make_pair(stream_name, column_data));
            files[stream_name].data_file_path = table_path + stream_name + DBMS_STORAGE_LOG_DATA_FILE_EXTENSION;
        }
    };

    IDataType::SubstreamPath substream_path;
    type.enumerateStreams(stream_callback, substream_path);
}


void StorageTinyLog::rename(const String & new_path_to_table_data, const String & new_database_name, const String & new_table_name, TableStructureWriteLockHolder &)
{
    std::unique_lock<std::shared_mutex> lock(rwlock);

    disk->moveDirectory(table_path, new_path_to_table_data);

    table_path = new_path_to_table_data;
    database_name = new_database_name;
    table_name = new_table_name;
    file_checker.setPath(table_path + "sizes.json");

    for (auto & file : files)
        file.second.data_file_path = table_path + fileName(file.second.data_file_path);
}


BlockInputStreams StorageTinyLog::read(
    const Names & column_names,
    const SelectQueryInfo & /*query_info*/,
    const Context & context,
    QueryProcessingStage::Enum /*processed_stage*/,
    const size_t max_block_size,
    const unsigned /*num_streams*/)
{
    check(column_names);
	// When reading, we lock the entire storage, because we only have one file
	// per column and can't modify it concurrently.
    return BlockInputStreams(1, std::make_shared<TinyLogBlockInputStream>(
        max_block_size, Nested::collect(getColumns().getAllPhysical().addTypes(column_names)), *this, context.getSettingsRef().max_read_buffer_size));
}


BlockOutputStreamPtr StorageTinyLog::write(
    const ASTPtr & /*query*/, const Context & /*context*/)
{
    return std::make_shared<TinyLogBlockOutputStream>(*this);
}


CheckResults StorageTinyLog::checkData(const ASTPtr & /* query */, const Context & /* context */)
{
    std::shared_lock<std::shared_mutex> lock(rwlock);
    return file_checker.check();
}

void StorageTinyLog::truncate(const ASTPtr &, const Context &, TableStructureWriteLockHolder &)
{
    if (table_name.empty())
        throw Exception("Logical error: table name is empty", ErrorCodes::LOGICAL_ERROR);

    std::unique_lock<std::shared_mutex> lock(rwlock);

    disk->clearDirectory(table_path);

    files.clear();
    file_checker = FileChecker{disk, table_path + "sizes.json"};

    for (const auto &column : getColumns().getAllPhysical())
        addFiles(column.name, *column.type);
}


void registerStorageTinyLog(StorageFactory & factory)
{
    factory.registerStorage("TinyLog", [](const StorageFactory::Arguments & args)
    {
        if (!args.engine_args.empty())
            throw Exception(
                "Engine " + args.engine_name + " doesn't support any arguments (" + toString(args.engine_args.size()) + " given)",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        return StorageTinyLog::create(
            args.context.getDefaultDisk(), args.relative_data_path, args.database_name, args.table_name, args.columns, args.constraints,
            args.attach, args.context.getSettings().max_compress_block_size);
    });
}

}

#include <string.h>
#include "mpack-expect.h"
#include "mpack-reader.h"
#include "mpack-writer.h"
#include "util/yyjson.h"
#include "message.h"

//------------------------------------------------------------------------------
// Utility function declarations
//------------------------------------------------------------------------------

/**
 * @brief Parses a map from a MessagePack reader.
 *
 * The function reads from the reader and populates the provided map.
 *
 * @param reader The MessagePack reader from which to parse the map.
 * @param map The map to populate with parsed key-value pairs.
 * @return True if the parsing was successful, false otherwise.
 */
static bool msg_parse_map(mpack_reader_t* reader, vws_kvs* map);

/**
 * @brief Parses content from a MessagePack reader into a buffer.
 *
 * The function reads from the reader and appends the data into the buffer.
 *
 * @param reader The MessagePack reader from which to parse the content.
 * @param buffer The buffer to which the parsed content will be appended.
 * @return An integer indicating the status of the operation.
 */
static int32_t msg_parse_content(mpack_reader_t* reader, vws_buffer* buffer);

//------------------------------------------------------------------------------
// API functions
//------------------------------------------------------------------------------

vrtql_msg* vrtql_msg_new()
{
    vrtql_msg* msg = vws.calloc(1, sizeof(vrtql_msg));

    msg->routing = vws_kvs_new(10, false);
    msg->headers = vws_kvs_new(10, false);

    msg->content = vws_buffer_new();
    msg->flags   = 0;
    msg->format  = VM_MPACK_FORMAT;
    msg->data    = NULL;

    vws_set_flag(&msg->flags, VM_MSG_VALID);

    return msg;
}

void vrtql_msg_clear(vrtql_msg* msg)
{
    vrtql_msg_clear_routings(msg);
    vrtql_msg_clear_headers(msg);
    vrtql_msg_clear_content(msg);
}

vrtql_msg* vrtql_msg_copy(vrtql_msg* original)
{
    if (original == NULL)
    {
        return NULL;
    }

    // Create a new message
    vrtql_msg* copy = vrtql_msg_new();

    // Copy routing table
    for (size_t i = 0; i < original->routing->used; i++)
    {
        vws_kvs_set_cstring( copy->routing,
                             original->routing->array[i].key,
                             original->routing->array[i].value.data );
    }

    // Copy headers table
    for (size_t i = 0; i < original->headers->used; i++)
    {
        vws_kvs_set_cstring( copy->headers,
                             original->headers->array[i].key,
                             original->headers->array[i].value.data );
    }

    // Copy content
    vws_buffer_append( copy->content,
                       (ucstr)original->content->data,
                       original->content->size );

    // Copy flags and format
    copy->flags  = original->flags;
    copy->format = original->format;
    copy->data   = original->data;

    return copy;
}

void vrtql_msg_free(vrtql_msg* msg)
{
    // Safety measure to prevent double freeing.
    if (msg == NULL)
    {
        return;
    }

    vws_kvs_free(msg->routing);
    vws_kvs_free(msg->headers);

    vws_buffer_free(msg->content);

    vws.free(msg);
    msg = NULL;
}

vws_buffer* vrtql_msg_serialize(vrtql_msg* msg)
{
    if (msg == NULL)
    {
        return false;
    }

    if (msg->format == VM_MPACK_FORMAT)
    {
        // Serialize MessagePack

        // Buffer to hold data
        vws_buffer* buffer = vws_buffer_new();

        // Initialize writer

        cstr key; cstr value;
        mpack_writer_t writer;
        mpack_writer_init_growable(&writer, (char**)&buffer->data, &buffer->size);

        // Binary is an array of 3 elements: routing, headers, content.
        mpack_start_array(&writer, 3);

        // Generate routing

        mpack_build_map(&writer);
        for (size_t i = 0; i < msg->routing->used; i++)
        {
            mpack_write_cstr(&writer, msg->routing->array[i].key);
            mpack_write_cstr(&writer, msg->routing->array[i].value.data);
        }
        mpack_complete_map(&writer);

        // Generate headers

        mpack_build_map(&writer);

        for (size_t i = 0; i < msg->headers->used; i++)
        {
            mpack_write_cstr(&writer, msg->headers->array[i].key);
            mpack_write_cstr(&writer, msg->headers->array[i].value.data);
        }
        mpack_complete_map(&writer);

        // Create content

        int size  = msg->content->size;
        cstr data = (cstr)msg->content->data;
        mpack_write_bin(&writer, data, size);

        // Close array
        mpack_finish_array(&writer);

        // Cleanup

        mpack_error_t rc = mpack_writer_destroy(&writer);

        if (rc != mpack_ok)
        {
            char buf[256];
            cstr text = mpack_error_to_string(rc);
            snprintf(buf, sizeof(buf), "Encoding errror: %s", text);
            vws.error(VE_RT, buf);

            vws_buffer_free(buffer);
            return NULL;
        }

        return buffer;
    }

    if (msg->format == VM_JSON_FORMAT)
    {
        // Serialize JSON

        // Buffer to hold data
        vws_buffer* buffer = vws_buffer_new();

        // Create a mutable doc
        yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);

        // We're generating an array as root.
        yyjson_mut_val* root = yyjson_mut_arr(doc);

        // Set root of the doc
        yyjson_mut_doc_set_root(doc, root);

        // Generate routing
        yyjson_mut_val* routing = yyjson_mut_arr_add_obj(doc, root);
        cstr key; cstr value;
        for (size_t i = 0; i < msg->routing->used; i++)
        {
            yyjson_mut_obj_add_str( doc,
                                    routing,
                                    msg->routing->array[i].key,
                                    msg->routing->array[i].value.data );
        }

        // Generate headers
        yyjson_mut_val* headers = yyjson_mut_arr_add_obj(doc, root);

        for (size_t i = 0; i < msg->routing->used; i++)
        {
            yyjson_mut_obj_add_str( doc,
                                    routing,
                                    msg->headers->array[i].key,
                                    msg->headers->array[i].value.data );
        }

        // Add content
        int size  = msg->content->size;
        cstr data = (cstr)msg->content->data;

        if (size > 0)
        {
            yyjson_mut_arr_add_strncpy(doc, root, data, size);
        }

        // To string, minified
        cstr json = yyjson_mut_write(doc, 0, NULL);

        if (json)
        {
            // Assuming vws_buffer_write takes a char* and size, writes the
            // data to the buffer
            vws_buffer_append(buffer, (ucstr)json, strlen(json));
            vws.free((void*)json);
        }

        // Free the doc
        yyjson_mut_doc_free(doc);

        return buffer;
    }

    return NULL;
}

bool vrtql_msg_deserialize(vrtql_msg* msg, ucstr data, size_t length)
{
    if ((data == NULL) || (length == 0))
    {
        return false;
    }

    // Parse message based on exptected format
    //
    // A message can be serialized into both JSON and MessagePack on the wire
    // within the same connection. That is, the protocol supports auto-detection
    // of JSON or MessagePack. The binary MessagePack format conveniently starts
    // with an invalid UTF-8 character which works as a magic number to identify
    // the binary message in the stream. This way a single connection can use
    // both JSON and binary MessageMack messages in same stream. The presence of
    // magic number signifies binary format and is therefore deserialized using
    // MessagePack. Absent that it parses UTF-8/JSON until encountering a NULL
    // terminator.
    //
    // The magic number comes from MessagePack binary format which is based on
    // the number of arguments (n) to be serialized. We are a format consisting
    // of an array with three elements: routing, headers, content. This means
    // the value fori n is 3. Therefore the MessagePack format for this object
    // would start with the value (0x90u | 3). This is our magic number which we
    // use to check if the data is MessagePack. Everything else is assumed to be
    // JSON.

    unsigned char magic_number = (unsigned char)(0x90u | 3);
    unsigned char first_byte   = (unsigned char)data[0];

    if (first_byte == magic_number)
    {
        // Deserialize MessagePack

        // Initialize reader
        mpack_reader_t reader;
        mpack_reader_init_data(&reader, (cstr)data, length);

        // Expect an array of size 3
        if (mpack_expect_array(&reader) != 3)
        {
            vws.error(VE_RT, "Invalid MessagePack format");
            return false;
        }

        // Parse routing map

        if (msg_parse_map(&reader, msg->routing) == false)
        {
            return false;
        }

        // Parse header map

        if (msg_parse_map(&reader, msg->headers) == false)
        {
            return false;
        }

        // Parse content

        ssize_t n = msg_parse_content(&reader, msg->content);

        if (n < 0)
        {
            return false;
        }
        else
        {
            msg->content->size = n;
        }

        // Finish reading the array
        mpack_done_array(&reader);

        // Cleanup

        mpack_error_t rc = mpack_reader_destroy(&reader);

        if (rc != mpack_ok)
        {
            char buf[256];
            cstr text = mpack_error_to_string(rc);
            snprintf(buf, sizeof(buf), "Decoding errror: %s", text);
            vws.error(VE_RT, buf);

            return false;
        }

        // Record format
        msg->format = VM_MPACK_FORMAT;
    }
    else
    {
        // Deserialize JSON

        yyjson_read_flag flags = YYJSON_READ_NOFLAG;
        yyjson_doc* doc        = yyjson_read((cstr)data, length, flags);
        yyjson_val* root       = yyjson_doc_get_root(doc);

        if (!yyjson_is_arr(root) || yyjson_arr_size(root) != 3)
        {
            vws.error(VE_RT, "Invalid JSON: Root is not an array of size 3");
            yyjson_doc_free(doc);

            return false;
        }

        yyjson_val* routing = yyjson_arr_get(root, 0);
        yyjson_val* headers = yyjson_arr_get(root, 1);
        yyjson_val* content = yyjson_arr_get(root, 2);

        yyjson_val* key;
        yyjson_val* value;
        yyjson_obj_iter iter;

        if (routing && yyjson_is_obj(routing))
        {
            yyjson_obj_iter_init(routing, &iter);

            while ((key = yyjson_obj_iter_next(&iter)))
            {
                value = yyjson_obj_iter_get_val(key);
                vws_kvs_set_cstring( msg->routing,
                                     yyjson_get_str(key),
                                     yyjson_get_str(value) );
            }
        }
        else
        {
            vws.error(VE_RT, "Invalid JSON: routing not JSON object");
            yyjson_doc_free(doc);

            return false;
        }

        if (headers && yyjson_is_obj(headers))
        {
            yyjson_obj_iter_init(headers, &iter);
            while ((key = yyjson_obj_iter_next(&iter)))
            {
                value = yyjson_obj_iter_get_val(key);
                vws_kvs_set_cstring( msg->headers,
                                     yyjson_get_str(key),
                                     yyjson_get_str(value) );
            }
        }
        else
        {
            vws.error(VE_RT, "Invalid JSON: headers is not JSON object");
            yyjson_doc_free(doc);

            return false;
        }

        if (content && yyjson_is_str(content))
        {
            vrtql_msg_set_content(msg, yyjson_get_str(content));
        }
        else
        {
            vws.error(VE_RT, "Invalid JSON: content is not a string");
            yyjson_doc_free(doc);

            return false;
        }

        yyjson_doc_free(doc);

        // Record format
        msg->format = VM_JSON_FORMAT;
    }

    return true;
}

bool vrtql_msg_is_empty(vrtql_msg* msg)
{
    if (msg->routing->used > 0)
    {
        return false;
    }

    if (msg->headers->used > 0)
    {
        return false;
    }

    if (msg->headers->used > 0)
    {
        return false;
    }

    if (msg->content->size > 0)
    {
        return false;
    }

    return true;
}

vws_buffer* vrtql_msg_repr(vrtql_msg* msg)
{
    // Buffer to hold data
    vws_buffer* buffer = vws_buffer_new();

    // Create a mutable doc
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);

    // We're generating an array as root.
    yyjson_mut_val* root = yyjson_mut_arr(doc);

    // Set root of the doc
    yyjson_mut_doc_set_root(doc, root);

    // Generate routing
    yyjson_mut_val* routing = yyjson_mut_arr_add_obj(doc, root);
    cstr key; cstr value;

    for (size_t i = 0; i < msg->routing->used; i++)
    {
        yyjson_mut_obj_add_str( doc,
                                routing,
                                msg->routing->array[i].key,
                                msg->routing->array[i].value.data );
    }

    // Generate headers
    yyjson_mut_val* headers = yyjson_mut_arr_add_obj(doc, root);
    for (size_t i = 0; i < msg->headers->used; i++)
    {
        yyjson_mut_obj_add_str( doc,
                                headers,
                                msg->headers->array[i].key,
                                msg->headers->array[i].value.data );
    }

    // To string, minified
    cstr json = yyjson_mut_write(doc, 0, NULL);

    if (json)
    {
        // Assuming vws_buffer_write takes a char* and size, writes the
        // data to the buffer
        vws_buffer_append(buffer, (ucstr)json, strlen(json));
        vws.free((void*)json);
    }

    // Free the doc
    yyjson_mut_doc_free(doc);

    if (msg->content->size > 0)
    {
        vws_buffer_append(buffer, msg->content->data, msg->content->size);
    }

    // Null terminate
    vws_buffer_append(buffer, "\0", 1);

    return buffer;
}

void vrtql_msg_dump(vrtql_msg* msg)
{
    vws_buffer* buffer = vrtql_msg_repr(msg);
    //printf("%.*s\n", buffer->size, buffer->data);
    fwrite(buffer->data, 1, buffer->size, stdout);
    putchar('\n');
    vws_buffer_free(buffer);
}

cstr vrtql_msg_get_header(vrtql_msg* msg, cstr key)
{
    return vws_kvs_get_cstring(msg->headers, key);
}

void vrtql_msg_set_header(vrtql_msg* msg, cstr key, cstr value)
{
    vws_kvs_set_cstring(msg->headers, key, value);
}

void vrtql_msg_clear_header(vrtql_msg* msg, cstr key)
{
    vws_kvs_remove(msg->headers, key);
}

void vrtql_msg_clear_headers(vrtql_msg* msg)
{
    vws_kvs_clear(msg->headers);
}

cstr vrtql_msg_get_routing(vrtql_msg* msg, cstr key)
{
    return vws_kvs_get_cstring(msg->routing, key);
}

void vrtql_msg_set_routing(vrtql_msg* msg, cstr key, cstr value)
{
    vws_kvs_set_cstring(msg->routing, key, value);
}

void vrtql_msg_clear_routing(vrtql_msg* msg, cstr key)
{
    vws_kvs_remove(msg->routing, key);
}

void vrtql_msg_clear_routings(vrtql_msg* msg)
{
    vws_kvs_clear(msg->routing);
}

void vrtql_msg_clear_content(vrtql_msg* msg)
{
    vws_buffer_clear(msg->content);
}

cstr vrtql_msg_get_content(vrtql_msg* msg)
{
    return (cstr)msg->content->data;
}

size_t vrtql_msg_get_content_size(vrtql_msg* msg)
{
    return msg->content->size;
}

void vrtql_msg_set_content(vrtql_msg* msg, cstr value)
{
    vws_buffer_clear(msg->content);
    vws_buffer_append(msg->content, (ucstr)value, strlen(value));
}

void vrtql_msg_set_content_binary(vrtql_msg* msg, cstr value, size_t size)
{
    vws_buffer_clear(msg->content);
    vws_buffer_append(msg->content, (ucstr)value, size);
}

ssize_t vrtql_msg_send(vws_cnx* c, vrtql_msg* msg)
{
    vws_buffer* binary = vrtql_msg_serialize(msg);
    ssize_t bytes      = vws_frame_send_binary(c, binary->data, binary->size);
    vws_buffer_free(binary);

    return bytes;
}

vrtql_msg* vrtql_msg_recv(vws_cnx* c)
{
    vws_msg* wsm = vws_msg_recv(c);

    if (wsm == NULL)
    {
        return NULL;
    }

    // Deserialize VRTQL message
    vrtql_msg* m = vrtql_msg_new();
    ucstr data   = wsm->data->data;
    size_t size  = wsm->data->size;
    if (vrtql_msg_deserialize(m, data, size) == false)
    {
        // Error already set
        vws_msg_free(wsm);
        vrtql_msg_free(m);
        return NULL;
    }

    vws_msg_free(wsm);

    return m;
}

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

bool msg_parse_map(mpack_reader_t* reader, vws_kvs* map)
{
    // Clear contents
    vws_kvs_clear(map);

    mpack_tag_t tag = mpack_read_tag(reader);

    if (mpack_reader_error(reader) != mpack_ok)
    {
        return false;
    }

    if (mpack_tag_type(&tag) != mpack_type_map)
    {
        return false;
    }

    char* key;
    char* value;

    uint32_t count = mpack_tag_map_count(&tag);

    while (count-- > 0)
    {
        uint32_t length;
        cstr data;
        mpack_tag_t tag;

        //> Get key

        tag = mpack_read_tag(reader);

        if (mpack_tag_type(&tag) != mpack_type_str)
        {
            printf("ERROR: key must be string\n");
            return false;
        }

        length = mpack_tag_str_length(&tag);
        data   = mpack_read_bytes_inplace(reader, length);
        key    = vws.malloc(length + 1);

        memcpy(key, data, length);
        key[length] = 0;
        mpack_done_str(reader);

        //> Get value

        tag = mpack_read_tag(reader);

        if (mpack_tag_type(&tag) != mpack_type_str)
        {
            printf("ERROR: value must be string\n");
            return false;
        }

        length = mpack_tag_str_length(&tag);
        data   = mpack_read_bytes_inplace(reader, length);
        value  = vws.malloc(length + 1);

        memcpy(value, data, length);
        value[length] = 0;
        mpack_done_str(reader);

        vws_kvs_set_cstring(map, key, value);

        vws.free(key);
        vws.free(value);

        if (mpack_reader_error(reader) != mpack_ok)
        {
            return false;
        }
    }

    mpack_done_map(reader);

    return true;
}

int32_t msg_parse_content(mpack_reader_t* reader, vws_buffer* buffer)
{
    mpack_tag_t tag = mpack_read_tag(reader);

    if (mpack_reader_error(reader) != mpack_ok)
    {
        return -1;
    }

    mpack_type_t type = mpack_tag_type(&tag);

    if ((type != mpack_type_bin) && (type != mpack_type_str))
    {
        return -1;
    }

    uint32_t length = mpack_tag_str_length(&tag);

    if (length == 0)
    {
        return 0;
    }

    cstr data = mpack_read_bytes_inplace(reader, length);

    vws_buffer_clear(buffer);
    vws_buffer_append(buffer, (ucstr)data, length);
    mpack_done_str(reader);

    return length;
}



//#include "config.h"

#include "ew-server.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>


enum {
  PROP_NAME = 1
};


static void ew_hls_handle_m3u8 (SoupServer *server, SoupMessage *msg,
    const char *path, GHashTable *query, SoupClientContext *client,
    gpointer user_data);
static void ew_hls_handle_stream_m3u8 (SoupServer *server, SoupMessage *msg,
    const char *path, GHashTable *query, SoupClientContext *client,
    gpointer user_data);

static void ew_hls_handle_ts_chunk (SoupServer *server, SoupMessage *msg,
    const char *path, GHashTable *query, SoupClientContext *client,
    gpointer user_data);
void ew_program_add_hls_chunk (EwServerStream *stream, SoupBuffer *buf);

static gboolean
sink_data_probe_callback (GstPad *pad, GstMiniObject *mo, gpointer user_data);

static void ew_hls_update_variant (EwProgram *program);

void
ew_server_stream_add_hls (EwServerStream *stream)
{
  EwProgram *program = stream->program;
  char *s;
  int mbs_per_sec;
  int level;
  int profile;

  if (!program->enable_hls) {

    program->hls.target_duration = 4;

    program->enable_hls = TRUE;

    s = g_strdup_printf ("/%s.m3u8", program->location);
    soup_server_add_handler (program->server->server, s, ew_hls_handle_m3u8,
        program, NULL);
    g_free (s);
  }

  gst_pad_add_data_probe (gst_element_get_pad (stream->sink, "sink"),
      G_CALLBACK(sink_data_probe_callback), stream);

  profile = 0;
  if (stream->type == EW_SERVER_STREAM_TS) {
    profile = 0x42e0; /* baseline */
  } else if (stream->type == EW_SERVER_STREAM_TS_MAIN) {
    profile = 0x4d40; /* main */
  }
  //profile = 0x6400; // high

  mbs_per_sec = ((stream->width + 15)>>4) * ((stream->height + 15)>>4) * 30;
  if (mbs_per_sec <= 1485) {
    level = 10;
  } else if (mbs_per_sec <= 3000) {
    level = 11;
  } else if (mbs_per_sec <= 6000) {
    level = 12;
  } else if (mbs_per_sec <= 19800) {
    level = 21;
  } else if (mbs_per_sec <= 20250) {
    level = 22;
  } else if (mbs_per_sec <= 40500) {
    level = 30;
  } else if (mbs_per_sec <= 108000) {
    level = 31;
  } else if (mbs_per_sec <= 216000) {
    level = 32;
  } else if (mbs_per_sec <= 245760) {
    level = 40;
  } else if (mbs_per_sec <= 522240) {
    level = 42;
  } else if (mbs_per_sec <= 589824) {
    level = 50;
  } else if (mbs_per_sec <= 983040) {
    level = 51;
  } else {
    level = 0xff;
  }

  stream->codecs = g_strdup_printf ("avc1.%04X%02X, mp4a.40.2", profile, level);
  stream->is_hls = TRUE;

  stream->adapter = gst_adapter_new ();

  s = g_strdup_printf ("/%s-%dx%d-%dkbps%s.m3u8", program->location,
        stream->width, stream->height, stream->bitrate/1000, stream->mod);
  soup_server_add_handler (program->server->server, s, ew_hls_handle_stream_m3u8,
      stream, NULL);
  g_free (s);

  ew_hls_update_variant (program);
}


typedef struct _ChunkCallback ChunkCallback;
struct _ChunkCallback {
  EwServerStream *stream;
  guint8 *data;
  int n;
};

gboolean
ew_program_add_hls_chunk_callback (gpointer data)
{
  ChunkCallback *chunk_callback = (ChunkCallback *)data;
  SoupBuffer *buffer;

  buffer = soup_buffer_new (SOUP_MEMORY_TAKE, chunk_callback->data, chunk_callback->n);
  ew_program_add_hls_chunk (chunk_callback->stream, buffer);

  g_free (chunk_callback);

  return FALSE;
}

static gboolean
sink_data_probe_callback (GstPad *pad, GstMiniObject *mo, gpointer user_data)
{
  EwServerStream *stream = (EwServerStream *) user_data;

  if (GST_IS_BUFFER (mo)) {
    GstBuffer *buffer = GST_BUFFER(mo);
    guint8 *data = GST_BUFFER_DATA(buffer);

    if (((data[3]>>4)&2) && ((data[5]>>6)&1)) {
      int n;

      //g_print("key frame\n");

      n = gst_adapter_available (stream->adapter);
      if (n < 188 * 100) {
        //g_print("skipped (too early)\n");
      } else {
        ChunkCallback *chunk_callback;

        chunk_callback = g_malloc0 (sizeof(ChunkCallback));
        chunk_callback->data = gst_adapter_take (stream->adapter, n);
        chunk_callback->n = n;
        chunk_callback->stream = stream;

        g_idle_add (ew_program_add_hls_chunk_callback, chunk_callback);
      }
    }

    gst_adapter_push (stream->adapter, gst_buffer_ref(buffer));
  } else {
    //g_print("got event\n");
  }

  return TRUE;
}

void
ew_program_add_hls_chunk (EwServerStream *stream, SoupBuffer *buf)
{
  EwHLSSegment *segment;

  segment = &stream->chunks[stream->n_chunks%N_CHUNKS];

  if (segment->buffer) {
    soup_server_remove_handler (stream->program->server->server,
        segment->location);
    g_free (segment->location);
    soup_buffer_free (segment->buffer);
  }
  segment->index = stream->n_chunks;
  segment->buffer = buf;
  segment->location = g_strdup_printf ("/%s-%dx%d-%dkbps%s-%05d.ts",
      stream->program->location, stream->width, stream->height,
      stream->bitrate/1000, stream->mod, stream->n_chunks);
  segment->duration = stream->program->hls.target_duration;

  stream->hls.need_index_update = TRUE;

  soup_server_add_handler (stream->program->server->server,
      segment->location,
      ew_hls_handle_ts_chunk, segment, NULL);

  stream->n_chunks++;
  stream->program->n_hls_chunks = stream->n_chunks;

  if (stream->n_chunks == 1) {
    ew_hls_update_variant (stream->program);
  }

}


static void
ew_hls_update_index (EwServerStream *stream)
{
  EwProgram *program = stream->program;
  GString *s;
  int i;
  int seq_num = MAX(0,program->n_hls_chunks - 5);

  //g_print ("seq_num %d\n", seq_num);

  s = g_string_new ("#EXTM3U\n");

  g_string_append_printf (s, "#EXT-X-TARGETDURATION:%d\n",
      program->hls.target_duration);
  g_string_append_printf (s, "#EXT-X-MEDIA-SEQUENCE:%d\n", seq_num);
  if (program->hls.is_encrypted) {
    g_string_append_printf (s, "#EXT-X-KEY:METHOD=AES-128,URI=\"%s\"",
        program->hls.key_uri);
    if (program->hls.have_iv) {
      g_string_append_printf (s, ",IV=0x%08x%08x%08x%08x",
          program->hls.init_vector[0], program->hls.init_vector[1],
          program->hls.init_vector[2], program->hls.init_vector[3]);
    } else {
      g_string_append (s, "\n");
    }
  } else {
    g_string_append (s, "#EXT-X-KEY:METHOD=NONE\n");
  }

  if (0) {
    g_string_append (s, "#EXT-X-PROGRAM-DATE-TIME:YYYY-MM-DDThh:mm:ssZ\n");
  }
  g_string_append (s, "#EXT-X-ALLOW-CACHE:NO\n");
  g_string_append (s, "#EXT-X-VERSION:1\n");

  for(i=seq_num; i<stream->n_chunks; i++){
    EwHLSSegment *segment = &stream->chunks[i%N_CHUNKS];

    g_string_append_printf (s,
        "#EXTINF:%d,\n"
        "%s%s\n",
        segment->duration,
        program->server->base_url,
        segment->location);
  }

  if (stream->hls.at_eos) {
    g_string_append (s, "#EXT-X-ENDLIST\n");
  }

  if (stream->hls.index_buffer) {
    soup_buffer_free (stream->hls.index_buffer);
  }
  stream->hls.index_buffer = soup_buffer_new (SOUP_MEMORY_TAKE, s->str, s->len);
  g_string_free (s, FALSE);

  stream->hls.need_index_update = FALSE;
}

static void
ew_hls_update_variant (EwProgram *program)
{
  GString *s;
  int j;

  s = g_string_new ("#EXTM3U\n");
  for(j=program->n_streams - 1;j>=0;j--){
  //for(j=0;j<program->n_streams;j++){
    if (!program->streams[j]->is_hls) continue;
    if (program->streams[j]->bitrate == 0) continue;
    if (program->streams[j]->n_chunks == 0) continue;

    g_string_append_printf (s,
        "#EXT-X-STREAM-INF:PROGRAM-ID=%d,BANDWIDTH=%d,"
        "CODECS=\"%s\",RESOLUTION=\"%dx%d\"\n",
        program->streams[j]->program_id,
        program->streams[j]->bitrate,
        program->streams[j]->codecs,
        program->streams[j]->width,
        program->streams[j]->height);
    g_string_append_printf (s, "%s/%s-%dx%d-%dkbps%s.m3u8\n",
        program->server->base_url,
        program->location,
        program->streams[j]->width,
        program->streams[j]->height,
        program->streams[j]->bitrate/1000,
        program->streams[j]->mod);
  }
  if (program->hls.variant_buffer) {
    soup_buffer_free (program->hls.variant_buffer);
  }
  program->hls.variant_buffer = soup_buffer_new (SOUP_MEMORY_TAKE, s->str, s->len);
  g_string_free (s, FALSE);

}

const char *
soup_message_get_uri_path (SoupMessage *msg)
{
  return soup_message_get_uri(msg)->path;
}

static void
ew_hls_handle_m3u8 (SoupServer *server, SoupMessage *msg,
    const char *path, GHashTable *query, SoupClientContext *client,
    gpointer user_data)
{
  EwProgram *program = (EwProgram *)user_data;

  g_assert (program->hls.variant_buffer != NULL);

  soup_message_set_status (msg, SOUP_STATUS_OK);
  soup_message_headers_replace (msg->response_headers,
      "Cache-Control", "no-cache");
  soup_message_headers_replace (msg->response_headers, "Content-Type",
      "video/x-mpegurl");
  soup_message_body_append_buffer (msg->response_body,
      program->hls.variant_buffer);
}

static void
ew_hls_handle_stream_m3u8 (SoupServer *server, SoupMessage *msg,
    const char *path, GHashTable *query, SoupClientContext *client,
    gpointer user_data)
{
  EwServerStream *stream = (EwServerStream *)user_data;

  if (stream->hls.index_buffer == NULL || stream->hls.need_index_update) {
    ew_hls_update_index (stream);
  }


  soup_message_set_status (msg, SOUP_STATUS_OK);
  soup_message_headers_replace (msg->response_headers, "Content-Type",
      "video/x-mpegurl");
  soup_message_headers_replace (msg->response_headers,
      "Cache-Control", "no-cache");
  soup_message_body_append_buffer (msg->response_body,
      stream->hls.index_buffer);
}

static void
ew_hls_handle_ts_chunk (SoupServer *server, SoupMessage *msg,
    const char *path, GHashTable *query, SoupClientContext *client,
    gpointer user_data)
{
  EwHLSSegment *segment = (EwHLSSegment *)user_data;

  soup_message_set_status (msg, SOUP_STATUS_OK);

  soup_message_headers_replace (msg->response_headers,
      "Cache-Control", "no-cache");
  soup_message_headers_replace (msg->response_headers, "Content-Type",
      "video/mp2t");

  soup_message_body_append_buffer (msg->response_body, segment->buffer);
}




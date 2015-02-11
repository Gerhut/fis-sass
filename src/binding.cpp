#include <nan.h>
#include <vector>
#include "sass_context_wrapper.h"

char* CreateString(Local<Value> value) {
  if (value->IsNull() || !value->IsString()) {
    return const_cast<char*>(""); // return empty string.
  }

  String::Utf8Value string(value);
  char *str = (char *)malloc(string.length() + 1);
  strcpy(str, *string);
  return str;
}

std::vector<sass_context_wrapper*> imports_collection;

struct Sass_Import** sass_importer2(const char* file, const char* prev, void* cookie)
{
  sass_context_wrapper* ctx_w = static_cast<sass_context_wrapper*>(cookie);

  TryCatch try_catch;

  imports_collection.push_back(ctx_w);

  Handle<Value> argv[] = {
    NanNew<String>(strdup(file ? strdup(file) : 0)),
    NanNew<String>(strdup(prev ? strdup(prev) : 0)),
    NanNew<Number>(imports_collection.size() - 1)
  };

  NanNew<Value>(ctx_w->importer_callback->Call(3, argv));

  if (try_catch.HasCaught()) {
    node::FatalException(try_catch);
  }

  return ctx_w->imports;
}

void ExtractOptions(Local<Object> options, void* cptr, sass_context_wrapper* ctx_w, bool isFile, bool isSync) {
  struct Sass_Context* ctx;

  NanAssignPersistent(ctx_w->result, options->Get(NanNew("result"))->ToObject());

  if (isFile) {
    ctx = sass_file_context_get_context((struct Sass_File_Context*) cptr);
    ctx_w->fctx = (struct Sass_File_Context*) cptr;
  }
  else {
    ctx = sass_data_context_get_context((struct Sass_Data_Context*) cptr);
    ctx_w->dctx = (struct Sass_Data_Context*) cptr;
  }

  struct Sass_Options* sass_options = sass_context_get_options(ctx);

  if (!isSync) {
    // ctx_w->request.data = ctx_w;

    // async (callback) style
    Local<Function> success_callback = Local<Function>::Cast(options->Get(NanNew("success")));
    Local<Function> error_callback = Local<Function>::Cast(options->Get(NanNew("error")));

    ctx_w->success_callback = new NanCallback(success_callback);
    ctx_w->error_callback = new NanCallback(error_callback);
  }

  Local<Function> importer_callback = Local<Function>::Cast(options->Get(NanNew("importer")));

  ctx_w->importer_callback = new NanCallback(importer_callback);

  if (!importer_callback->IsUndefined()) {
    sass_option_set_importer(sass_options, sass_make_importer(sass_importer2, ctx_w));
  }

  sass_option_set_input_path(sass_options, CreateString(options->Get(NanNew("file"))));
  sass_option_set_output_path(sass_options, CreateString(options->Get(NanNew("outFile"))));
  sass_option_set_image_path(sass_options, CreateString(options->Get(NanNew("imagePath"))));
  sass_option_set_output_style(sass_options, (Sass_Output_Style)options->Get(NanNew("style"))->Int32Value());
  sass_option_set_is_indented_syntax_src(sass_options, options->Get(NanNew("indentedSyntax"))->BooleanValue());
  sass_option_set_source_comments(sass_options, options->Get(NanNew("comments"))->BooleanValue());
  sass_option_set_omit_source_map_url(sass_options, options->Get(NanNew("omitSourceMapUrl"))->BooleanValue());
  sass_option_set_source_map_embed(sass_options, options->Get(NanNew("sourceMapEmbed"))->BooleanValue());
  sass_option_set_source_map_contents(sass_options, options->Get(NanNew("sourceMapContents"))->BooleanValue());
  sass_option_set_source_map_file(sass_options, CreateString(options->Get(NanNew("sourceMap"))));
  sass_option_set_include_path(sass_options, CreateString(options->Get(NanNew("paths"))));
  sass_option_set_precision(sass_options, options->Get(NanNew("precision"))->Int32Value());
}

void GetStats(Handle<Object> result, Sass_Context* ctx) {
  char** included_files = sass_context_get_included_files(ctx);
  Handle<Array> arr = NanNew<Array>();

  if (included_files) {
    for (int i = 0; included_files[i] != nullptr; ++i) {
      arr->Set(i, NanNew<String>(included_files[i]));
    }
  }

  (*result)->Get(NanNew("stats"))->ToObject()->Set(NanNew("includedFiles"), arr);
}

void GetSourceMap(Handle<Object> result, Sass_Context* ctx) {
  Handle<Value> source_map;

  if (sass_context_get_error_status(ctx)) {
    return;
  }

  if (sass_context_get_source_map_string(ctx)) {
    source_map = NanNew<String>(sass_context_get_source_map_string(ctx));
  }
  else {
    source_map = NanNew<String>("{}");
  }

  (*result)->Set(NanNew("sourceMap"), source_map);
}

int GetResult(Handle<Object> result, Sass_Context* ctx) {
  int status = sass_context_get_error_status(ctx);

  if (status == 0) {
    (*result)->Set(NanNew("css"), NanNew<String>(sass_context_get_output_string(ctx)));
    GetStats(result, ctx);
    GetSourceMap(result, ctx);
  }

  return status;
}

NAN_METHOD(RenderSync) {
  NanScope();

  Local<Object> options = args[0]->ToObject();
  char* source_string = CreateString(options->Get(NanNew("data")));
  struct Sass_Data_Context* dctx = sass_make_data_context(source_string);
  struct Sass_Context* ctx = sass_data_context_get_context(dctx);
  sass_context_wrapper* ctx_w = sass_make_context_wrapper();

  ExtractOptions(options, dctx, ctx_w, false, true);
  compile_data(dctx);

  int result = GetResult(Local<Object>::New(Isolate::GetCurrent(), ctx_w->result), ctx);
  Local<String> error;

  if (result != 0) {
    error = NanNew<String>(sass_context_get_error_json(ctx));
  }

  sass_free_context_wrapper(ctx_w);
  free(source_string);

  if (result != 0) {
    NanThrowError(error);
  }

  NanReturnValue(NanNew<Boolean>(result == 0));
}

NAN_METHOD(ImportedCallback) {
  NanScope();

  TryCatch try_catch;

  Local<Object> options = args[0]->ToObject();
  Local<Value> returned_value = options->Get(NanNew("objectLiteral"));
  size_t index = options->Get(NanNew("index"))->Int32Value();

  if (index >= imports_collection.size()) {
    NanReturnUndefined();
  }

  sass_context_wrapper* ctx_w = imports_collection[index];

  if (returned_value->IsArray()) {
    Handle<Array> array = Handle<Array>::Cast(returned_value);

    ctx_w->imports = sass_make_import_list(array->Length());

    for (size_t i = 0; i < array->Length(); ++i) {
      Local<Value> value = array->Get(i);

      if (!value->IsObject())
        continue;

      Local<Object> object = Local<Object>::Cast(value);
      char* path = CreateString(object->Get(String::NewFromUtf8(Isolate::GetCurrent(), "file")));
      char* contents = CreateString(object->Get(String::NewFromUtf8(Isolate::GetCurrent(), "contents")));

      ctx_w->imports[i] = sass_make_import_entry(path, (!contents || contents[0] == '\0') ? 0 : strdup(contents), 0);
    }
  }
  else if (returned_value->IsObject()) {
    ctx_w->imports = sass_make_import_list(1);
    Local<Object> object = Local<Object>::Cast(returned_value);
    char* path = CreateString(object->Get(String::NewFromUtf8(Isolate::GetCurrent(), "file")));
    char* contents = CreateString(object->Get(String::NewFromUtf8(Isolate::GetCurrent(), "contents")));

    ctx_w->imports[0] = sass_make_import_entry(path, (!contents || contents[0] == '\0') ? 0 : strdup(contents), 0);
  }
  else {
    ctx_w->imports = sass_make_import_list(1);
    ctx_w->imports[0] = sass_make_import_entry(ctx_w->file, 0, 0);
  }

  if (try_catch.HasCaught()) {
    node::FatalException(try_catch);
  }

  NanReturnValue(NanNew<Number>(0));
}

void RegisterModule(v8::Handle<v8::Object> target) {
  NODE_SET_METHOD(target, "renderSync", RenderSync);
  NODE_SET_METHOD(target, "importedCallback", ImportedCallback);
}

NODE_MODULE(binding, RegisterModule);

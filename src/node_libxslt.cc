#define BUILDING_NODE_EXTENSION
#include <iostream>
#include <node.h>
#include <nan.h>
#include <libxslt/xslt.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>

// includes from libxmljs
#include <xml_syntax_error.h>
#include <xml_document.h>

#include "./node_libxslt.h"
#include "./stylesheet.h"

using namespace v8;

NAN_METHOD(StylesheetSync) {
  	Nan::HandleScope scope;

    // From libxml document
    libxmljs::XmlDocument* doc = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[0]->ToObject());
    // From string
    //libxmljs::XmlDocument* doc = libxmljs::XmlDocument::FromXml(info);

    xsltStylesheetPtr stylesheet = xsltParseStylesheetDoc(doc->xml_obj);
    // TODO fetch actual error.
    if (!stylesheet) {
        return Nan::ThrowError("Could not parse XML string as XSLT stylesheet");
    }

    Local<Object> stylesheetWrapper = Stylesheet::New(stylesheet);
  	info.GetReturnValue().Set(stylesheetWrapper);
}

// for memory the segfault i previously fixed were due to xml documents being deleted
// by garbage collector before their associated stylesheet.
class StylesheetWorker : public Nan::AsyncWorker {
 public:
  StylesheetWorker(libxmljs::XmlDocument* doc, Nan::Callback *callback)
    : Nan::AsyncWorker(callback), doc(doc) {}
  ~StylesheetWorker() {}

  // Executed inside the worker-thread.
  // It is not safe to access V8, or V8 data structures
  // here, so everything we need for input and output
  // should go on `this`.
  void Execute () {
    libxmljs::WorkerSentinel workerSentinel(workerParent);
    result = xsltParseStylesheetDoc(doc->xml_obj);
  }

  // Executed when the async work is complete
  // this function will be run inside the main event loop
  // so it is safe to use V8 again
  void HandleOKCallback () {
    Nan::HandleScope scope;
    if (!result) {
        Local<Value> argv[] = { Nan::Error("Failed to parse stylesheet") };
        callback->Call(2, argv);
    } else {
        Local<Object> resultWrapper = Stylesheet::New(result);
        Local<Value> argv[] = { Nan::Null(), resultWrapper };
        callback->Call(2, argv);
    }
  };

 private:
  libxmljs::WorkerParent workerParent;
  libxmljs::XmlDocument* doc;
  xsltStylesheetPtr result;
};

NAN_METHOD(StylesheetAsync) {
    Nan::HandleScope scope;
    libxmljs::XmlDocument* doc = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[0]->ToObject());
    Nan::Callback *callback = new Nan::Callback(info[1].As<Function>());
    StylesheetWorker* worker = new StylesheetWorker(doc, callback);
    worker->SaveToPersistent("doc", info[0]);
    Nan::AsyncQueueWorker(worker);
    return;
}

// duplicate from https://github.com/bsuh/node_xslt/blob/master/node_xslt.cc
void freeArray(char **array, int size) {
    for (int i = 0; i < size; i++) {
        free(array[i]);
    }
    free(array);
}
// transform a v8 array into a char** to pass params to xsl transform
// inspired by https://github.com/bsuh/node_xslt/blob/master/node_xslt.cc
char** PrepareParams(Handle<Array> array) {
    uint32_t arrayLen = array->Length();
    char** params = (char **)malloc(sizeof(char *) * (arrayLen + 1));
    memset(params, 0, sizeof(char *) * (array->Length() + 1));
    for (unsigned int i = 0; i < array->Length(); i++) {
        Local<String> param = array->Get(Nan::New<Integer>(i))->ToString();
        params[i] = (char *)malloc(sizeof(char) * (param->Utf8Length() + 1));
        param->WriteUtf8(params[i]);
    }
    return params;
}

NAN_METHOD(ApplySync) {
    Nan::HandleScope scope;

    Stylesheet* stylesheet = Nan::ObjectWrap::Unwrap<Stylesheet>(info[0]->ToObject());
    libxmljs::XmlDocument* docSource = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[1]->ToObject());
    Handle<Array> paramsArray = Handle<Array>::Cast(info[2]);
    libxmljs::XmlDocument* docResult = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[3]->ToObject());

    char** params = PrepareParams(paramsArray);

    xmlDoc* result = xsltApplyStylesheet(stylesheet->stylesheet_obj, docSource->xml_obj, (const char **)params);
    if (!result) {
        freeArray(params, paramsArray->Length());
        return Nan::ThrowError("Failed to apply stylesheet");
    }

    // for some obscure reason I didn't manage to create a new libxmljs document in applySync,
	// but passing a document by reference and modifying its content works fine
    // replace the empty document in docResult with the result of the stylesheet
	docResult->xml_obj->_private = NULL;
    xmlFreeDoc(docResult->xml_obj);
    docResult->xml_obj = result;
    result->_private = docResult;

    freeArray(params, paramsArray->Length());

  	return;
}

// for memory the segfault i previously fixed were due to xml documents being deleted
// by garbage collector before their associated stylesheet.
class ApplyWorker : public Nan::AsyncWorker {
 public:
   // apply to String constructor
  ApplyWorker(Stylesheet* stylesheet, libxmljs::XmlDocument* docSource, char** params, int paramsLength, Nan::Callback *callback)
    : Nan::AsyncWorker(callback), stylesheet(stylesheet), docSource(docSource), params(params), paramsLength(paramsLength), docResult(NULL) {}
  ApplyWorker(Stylesheet* stylesheet, libxmljs::XmlDocument* docSource, char** params, int paramsLength, libxmljs::XmlDocument* docResult, Nan::Callback *callback)
    : Nan::AsyncWorker(callback), stylesheet(stylesheet), docSource(docSource), params(params), paramsLength(paramsLength), docResult(docResult) {}
  ~ApplyWorker() {}

  // Executed inside the worker-thread.
  // It is not safe to access V8, or V8 data structures
  // here, so everything we need for input and output
  // should go on `this`.
  void Execute () {
    libxmljs::WorkerSentinel workerSentinel(workerParent);
    result = xsltApplyStylesheet(stylesheet->stylesheet_obj, docSource->xml_obj, (const char **)params);
  }

  // Executed when the async work is complete
  // this function will be run inside the main event loop
  // so it is safe to use V8 again
  void HandleOKCallback () {
    Nan::HandleScope scope;

    if (!result) {
        Local<Value> argv[] = { Nan::Error("Failed to apply stylesheet") };
        freeArray(params, paramsLength);
        callback->Call(2, argv);
    } else if (docResult) {
        Local<Value> argv[] = { Nan::Null() };

        // for some obscure reason I didn't manage to create a new libxmljs document in applySync,
        // but passing a document by reference and modifying its content works fine
        // replace the empty document in docResult with the result of the stylesheet
        docResult->xml_obj->_private = NULL;
        xmlFreeDoc(docResult->xml_obj);
        docResult->xml_obj = result;
        result->_private = docResult;

        freeArray(params, paramsLength);

        callback->Call(1, argv);
    } else {// apply assync to string
        unsigned char* resStr;
        int len;
        int cnt=xsltSaveResultToString(&resStr,&len,result,stylesheet->stylesheet_obj);
        freeArray(params, paramsLength);
        xmlFreeDoc(result);
        if (cnt==-1) {
            Local<Value> argv[] = { Nan::Error("Failed to apply stylesheet") };
            callback->Call(2, argv);
        } else {
          Local<Value> argv[] = { Nan::Null(),Nan::New<String>((char*)resStr).ToLocalChecked()};
          callback->Call(2, argv);
        }
    }
  };

 private:
  libxmljs::WorkerParent workerParent;
  Stylesheet* stylesheet;
  libxmljs::XmlDocument* docSource;
  char** params;
  int paramsLength;
  libxmljs::XmlDocument* docResult;
  xmlDoc* result;
};

NAN_METHOD(ApplyAsync) {
    Nan::HandleScope scope;

    Stylesheet* stylesheet = Nan::ObjectWrap::Unwrap<Stylesheet>(info[0]->ToObject());
    libxmljs::XmlDocument* docSource = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[1]->ToObject());
    Handle<Array> paramsArray = Handle<Array>::Cast(info[2]);
    libxmljs::XmlDocument* docResult = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[3]->ToObject());
    Nan::Callback *callback = new Nan::Callback(info[4].As<Function>());

    char** params = PrepareParams(paramsArray);

    ApplyWorker* worker = new ApplyWorker(stylesheet, docSource, params, paramsArray->Length(), docResult, callback);
    for (uint32_t i = 0; i < 4; ++i) worker->SaveToPersistent(i, info[i]);
    Nan::AsyncQueueWorker(worker);
    return;
}

// process target doc and return raw string (in case the result is not a xml derivate)
// this is the way to choose if omit-xml-declaration is to be respected
NAN_METHOD(ApplySyncToString) {
    Nan::HandleScope scope;

    Stylesheet* stylesheet = Nan::ObjectWrap::Unwrap<Stylesheet>(info[0]->ToObject());
    libxmljs::XmlDocument* docSource = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[1]->ToObject());
    Handle<Array> paramsArray = Handle<Array>::Cast(info[2]);

    char** params = PrepareParams(paramsArray);
    unsigned char* resStr;
    int len;
    xmlDocPtr res = xsltApplyStylesheet(stylesheet->stylesheet_obj, docSource->xml_obj, (const char **)params);
    int cnt=xsltSaveResultToString(&resStr,&len,res,stylesheet->stylesheet_obj);
    xmlFreeDoc(res);
    freeArray(params, paramsArray->Length());
    if (cnt==-1) return Nan::ThrowError("Failed to apply stylesheet");
    else  info.GetReturnValue().Set(Nan::New<String>((char*)resStr).ToLocalChecked());
}

NAN_METHOD(ApplyAsyncToString) {
  Nan::HandleScope scope;

  Stylesheet* stylesheet = Nan::ObjectWrap::Unwrap<Stylesheet>(info[0]->ToObject());
  libxmljs::XmlDocument* docSource = Nan::ObjectWrap::Unwrap<libxmljs::XmlDocument>(info[1]->ToObject());
  Handle<Array> paramsArray = Handle<Array>::Cast(info[2]);
  Nan::Callback *callback = new Nan::Callback(info[3].As<Function>());

    char** params = PrepareParams(paramsArray);

    Nan::AsyncQueueWorker(new ApplyWorker(stylesheet, docSource, params, paramsArray->Length(), callback));
    return;
}

NAN_METHOD(RegisterEXSLT) {
    exsltRegisterAll();
    return;
}

// Compose the module by assigning the methods previously prepared
void InitAll(Handle<Object> exports) {
  	Stylesheet::Init(exports);
  	exports->Set(Nan::New<String>("stylesheetSync").ToLocalChecked(), Nan::New<FunctionTemplate>(StylesheetSync)->GetFunction());
    exports->Set(Nan::New<String>("stylesheetAsync").ToLocalChecked(), Nan::New<FunctionTemplate>(StylesheetAsync)->GetFunction());
  	exports->Set(Nan::New<String>("applySync").ToLocalChecked(), Nan::New<FunctionTemplate>(ApplySync)->GetFunction());
    exports->Set(Nan::New<String>("applyAsync").ToLocalChecked(), Nan::New<FunctionTemplate>(ApplyAsync)->GetFunction());
    exports->Set(Nan::New<String>("applySyncToString").ToLocalChecked(), Nan::New<FunctionTemplate>(ApplySyncToString)->GetFunction());
    exports->Set(Nan::New<String>("applyAsyncToString").ToLocalChecked(), Nan::New<FunctionTemplate>(ApplyAsyncToString)->GetFunction());
    exports->Set(Nan::New<String>("registerEXSLT").ToLocalChecked(), Nan::New<FunctionTemplate>(RegisterEXSLT)->GetFunction());
}
NODE_MODULE(node_libxslt, InitAll);

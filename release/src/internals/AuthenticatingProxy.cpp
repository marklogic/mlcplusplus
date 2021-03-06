/*
 * Copyright (c) MarkLogic Corporation. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *  AuthenticatingProxy.cpp
 *  Created by Paul Hoehne on 29 May 2014.
 *  Modified by Adam Fowler on 21 Apr 2016
 */


// our API includes
#include "mlclient/internals/AuthenticatingProxy.hpp"
#include "mlclient/internals/Credentials.hpp"

#include "mlclient/NoCredentialsException.hpp"
#include "mlclient/Response.hpp"
#include "mlclient/HttpHeaders.hpp"
#include "mlclient/DocumentSet.hpp"

#include "mlclient/logging.hpp"


// JSON and HTTP includes
#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <cpprest/http_headers.h>
#include <cpprest/base_uri.h>
#include <cpprest/interopstream.h>

// XML includes
#include "mlclient/ext/pugixml/pugixml.hpp"

#include <string>
#include <iostream>
#include <istream>


namespace mlclient {

namespace internals {
using namespace web::http;

const utility::string_t AUTHORIZATION_HEADER_NAME = U("Authorization");
const utility::string_t WWW_AUTHENTICATE_HEADER = U("WWW-Authenticate");
const std::string WWW_AUTHENTICATE_HEADER_INT = "WWW-Authenticate";

const std::string DEFAULT_KEY = "__DEFAULT";

using namespace utility;                    // Common utilities like string conversions
using namespace utility::conversions;       // String conversions
using namespace web;                        // Common features like URIs.
using namespace web::http;                  // Common HTTP functionality
using namespace web::http::client;          // HTTP client features
//using namespace concurrency::streams;       // Asynchronous streams
using namespace mlclient;

AuthenticatingProxy::AuthenticatingProxy() : credentials(),attempts(0),restMutex()
{
}

void AuthenticatingProxy::copyHeaders(const web::http::http_headers& from, mlclient::HttpHeaders& to) {
  std::map<std::string,std::string> headers;
  LOG(DEBUG) << "Headers:-";
  for (auto& it : from) {
    LOG(DEBUG) << "  Header: " << utility::conversions::to_utf8string(it.first) << " = " << utility::conversions::to_utf8string(it.second);
    to.setHeader(utility::conversions::to_utf8string(it.first), utility::conversions::to_utf8string(it.second));
  }
}


void AuthenticatingProxy::addCredentials(const internals::Credentials &c)
{
  credentials = c;
}

const Credentials& AuthenticatingProxy::getCredentials() const {
  return credentials;
}

Response* AuthenticatingProxy::doRequest(const std::string& method,const std::string& host,const std::string& path,const HttpHeaders& headers, const IDocumentContent* body) {

  TIMED_FUNC(AuthenticatingProxy_doRequest);
  LOG(DEBUG) << "doRequest: method: " << method << " host: " << host << " path: " << path;

  http_client raw_client(utility::conversions::to_string_t(host)); // TODO can we re-use these???

  std::string responseAuthHeaderValue = "";

  std::map<std::string,std::string> hs = headers.getHeaders();

  // TODO can we already pre-authenticate?

  bool authorised = true;

  utility::string_t bodyString;
  utility::string_t mimeString;


  try {
    http::http_request req(utility::conversions::to_string_t(method)); // TODO can we re-use these???
    http_headers& restHeaders = req.headers(); // MUST BE A REFERENCE - DO NOT INVOKE COPY CONSTRUCTOR!!!
    // copy additional headers - e.g. Accept: or Content-type: (For POST/PUT)
    for (auto& iter : hs) {
      if (restHeaders.has(utility::conversions::to_string_t(iter.first))) { // TODO verify that map doesn't handle duplicates for us. If it does, remove this check.
        restHeaders.remove(utility::conversions::to_string_t(iter.first));
      }
      restHeaders.add(utility::conversions::to_string_t(iter.first), utility::conversions::to_string_t(iter.second));
    }
    // TODO common string constants to pre-created variables
    if (!restHeaders.has(U("Accept"))) {
      restHeaders.add(U("Accept"),U(IDocumentContent::MIME_JSON)); // default to JSON response type for MarkLogic
    }
    req.set_request_uri(web::uri(utility::conversions::to_string_t(path)));

    if (credentials.canAuthenticate()) {
      LOG(DEBUG) << "Attempting to authenticate on first attempt";
      restHeaders.add(AUTHORIZATION_HEADER_NAME, utility::conversions::to_string_t(credentials.authenticate(("" + method), path)));
    } else {
      LOG(DEBUG) << "Not authenticating on first attempt";
    }

    if (nullptr != body) {
      bodyString = utility::conversions::to_string_t(body->getContent());
      mimeString = utility::conversions::to_string_t(body->getMimeType());
      // GOD AWFUL HACK
      if (utility::conversions::to_string_t("multipart/mime") == mimeString) {
        mimeString = utility::conversions::to_string_t("multipart/mime; boundary=BOUNDARY");
      }
      LOG(DEBUG) << "Body is not null on FIRST try";
      LOG(DEBUG) << "  mimeString: " << utility::conversions::to_utf8string(mimeString);
      LOG(DEBUG) << "  bodyString: " << utility::conversions::to_utf8string(bodyString);
      // TODO Any way to stream the below rather than convert in memory?
      req.set_body(bodyString,mimeString);
      //req.set_body(*(body->getStream()),utility::conversions::to_string_t(body->getMimeType()));
    }

    /*
    std::map<std::string,std::string>::const_iterator iter;
    // print out headers for debug
    LOG(DEBUG) << "Listing initial request headers:-";
    for (auto& iter : req.headers()) {
      LOG(DEBUG) << " header: " << iter.first << " = " << iter.second;
    }
    LOG(DEBUG) << "Finishing listing request headers.";
    */

    http_response raw_response; // TODO investigate if assignment uses move semantics for speed

    { // PERFORMANCE BRACE
      TIMED_SCOPE(AuthenticatingProxy_doRequest, "cpprest_httpclient_request");

      // Hold a mutex so that MarkLogic does not hit a DEADLOCK concurrent lock REST issue
      std::unique_lock<std::mutex> lck (restMutex,std::defer_lock);
      lck.lock();
      pplx::task<http_response> hr = raw_client.request(req);
      //LOG(DEBUG) << "Request body: " << utility::conversions::to_utf8string(req.to_string());

      raw_response = hr.get();
      lck.unlock();

    } // PERFORMANCE BRACE
    try
    {
      Response* response = new Response;
      response->setResponseCode((ResponseCode)raw_response.status_code());
      HttpHeaders h;
      AuthenticatingProxy::copyHeaders(raw_response.headers(),h);
      response->setResponseHeaders(h); // also sets response type via Content-type header

      std::vector<unsigned char> vec = raw_response.extract_vector().get();
      std::string str;
      str.reserve(vec.size());
      str.assign(vec.begin(),vec.end());
      response->setContent(str);
      /*
      if (response->getResponseType() == ResponseType::BINARY) {
        LOG(DEBUG) << "AuthenticatingProxy - Got binary response";
        std::vector<unsigned char> vec = raw_response.extract_vector().get();
        // convert to binary content, or store for now...

        std::string* str = new std::string;
        str->reserve(vec.size());
        str->assign(vec.begin(),vec.end());
        response->setContent(str);


        std::string str;
        str.reserve(vec.size());
        str.assign(vec.begin(),vec.end());
        response->setContent(str);
      } else {
        LOG(DEBUG) << "AuthenticatingProxy - Got text response";
        std::vector<unsigned char> vec = raw_response.extract_vector().get();
        std::string str;
        str.reserve(vec.size());
        str.assign(vec.begin(),vec.end());
        response->setContent(str);
        //response->setContent(new std::string(utility::conversions::to_utf8string(raw_response.extract_string().get())));
      }
      */
      responseAuthHeaderValue = utility::conversions::to_utf8string(raw_response.headers()[WWW_AUTHENTICATE_HEADER]);

      /*
      LOG(DEBUG) << "Listing response object headers:-";
      for (auto& iter : response->getResponseHeaders().getHeaders()) {
        LOG(DEBUG) << " header name: " << iter.first;
        LOG(DEBUG) << "  value = " << iter.second;
      }
      LOG(DEBUG) << "Finishing listing response object headers.";
      */

      //std::unique_ptr<std::string> c(new std::string(raw_response.extract_string().get()));
      // TODO replace with setContent(StringDocument()) ??? would it avoid a conversion at all???
      //response->setContent(new std::string(utility::conversions::to_utf8string(raw_response.extract_string().get()))); // TODO handle binary response types

      if (response->getResponseCode() != ResponseCode::UNAUTHORIZED) {
        return response;
      } else {
        authorised = false;
      }

    }
    catch (const http_exception& e)
    {
      // Print error.
      LOG(DEBUG) << "There was an error extracting content from response: " << e.what();
    }

  } catch(std::exception &e) {
    LOG(DEBUG) << "Exception " << e.what();
    //std::cerr << e.what() << std::endl;
  }

  if (!authorised) {
    LOG(DEBUG) << "Unauthorised. Retrying...";
    //LOG(DEBUG) << "UNAUTHORIZED";
    /*
    LOG(DEBUG) << "Listing headers fetched from response:-";
    for (auto& iter : response_headers.getHeaders()) {
      LOG(DEBUG) << " header: " << iter.first << " = " << iter.second;
    }
    LOG(DEBUG) << "Finishing listing headers fetched from response.";
    */

    // TODO I don't think the following is ever called... verify it.
    // TODO verify if the first request used is a POST, the following does not error (IT SPECIFIED GET AS THE METHOD!!!)
    try {
      http::http_request req(utility::conversions::to_string_t(method));
      req.set_request_uri(web::uri(utility::conversions::to_string_t(path)));

      // print out all headers from response
/*
      LOG(DEBUG) << "Listing Unauth response headers:-";
      for (auto& iter : response_headers.getHeaders()) {
        LOG(DEBUG) << " header: " << iter.first << " = " << iter.second;
      }
      LOG(DEBUG) << "Finishing listing unauth response headers.";
      LOG(DEBUG) << "Value of www authenticate: " << WWW_AUTHENTICATE_HEADER;
      */


      std::map<std::string,std::string>::const_iterator iter;
      std::map<std::string,std::string> hs = headers.getHeaders();
      http_headers& restHeaders = req.headers(); // MUST BE A REFERENCE - DO NOT INVOKE COPY CONSTRUCTOR!!!
      for (auto& iter : hs) {
        if (restHeaders.has(utility::conversions::to_string_t(iter.first))) { // TODO verify that map doesn't handle duplicates for us. If it does, remove this check.
          restHeaders.remove(utility::conversions::to_string_t(iter.first));
        }
        restHeaders.add(utility::conversions::to_string_t(iter.first), utility::conversions::to_string_t(iter.second));
      }
      // TODO common string constants to pre-created variables
      if (!restHeaders.has(U("Accept"))) {
        restHeaders.add(U("Accept"),U(IDocumentContent::MIME_JSON)); // default to JSON response type for MarkLogic
      }
      LOG(DEBUG) << "Generating auth header";
      LOG(DEBUG) << "Original auth response was: " << responseAuthHeaderValue;
      std::string av(credentials.authenticate(("" + method), path, responseAuthHeaderValue));
      LOG(DEBUG) << "Auth header: " << av;
      LOG(DEBUG) << "Auth header name: " << utility::conversions::to_utf8string(AUTHORIZATION_HEADER_NAME);
      restHeaders.add(
          AUTHORIZATION_HEADER_NAME,
        utility::conversions::to_string_t(av)
      );
      LOG(DEBUG) << "Start of request headers";
      for (auto& iter : req.headers()) {
        LOG(DEBUG) << "  Request Header: " << utility::conversions::to_utf8string(iter.first) << "='" << utility::conversions::to_utf8string(iter.second) << "'";
      }
      LOG(DEBUG) << "End of request headers";

      if (nullptr != body) {
        LOG(DEBUG) << "Request body is not null on retry";
        LOG(DEBUG) << "  mimeString: " << utility::conversions::to_utf8string(mimeString);
        LOG(DEBUG) << "  bodyString: " << utility::conversions::to_utf8string(bodyString);
        req.set_body(bodyString,mimeString);
        //req.set_body(utility::conversions::to_string_t(body->getContent()), utility::conversions::to_string_t(body->getMimeType()));
        //concurrency::streams::stdio_istream
        //std::istream* isp = body->getStream();
        //concurrency::streams::stdio_istream is(*isp);
        //req.set_body(is, utility::conversions::to_string_t(body->getMimeType()));

        //std::ifstream InFile( FileName, std::ifstream::binary );
        //std::vector<unsigned char> data( ( std::istreambuf_iterator<unsigned char>( *isp ) ), std::istreambuf_iterator<unsigned char>() );
        //req.headers().add(U("Content-type"),utility::conversions::to_string_t(body->getMimeType()));
        //req.set_body(data);
      } else {
        LOG(DEBUG) << "Request body IS null on retry";
      }
      /*
      LOG(DEBUG) << "Listing request headers post auth calculation:-";
      for (auto& iter : req.headers()) {
        LOG(DEBUG) << " header: " << iter.first << " = " << iter.second;
      }
      LOG(DEBUG) << "Finishing listing request headers.";
      */

      http_response raw_response;// = raw_client.request(req).get();
      { // PERFORMANCE BRACE
        TIMED_SCOPE(AuthenticatingProxy_doRequest, "cpprest_httpclient_request");
        std::unique_lock<std::mutex> lck (restMutex,std::defer_lock);
        lck.lock();
        pplx::task<http_response> hr = raw_client.request(req);
        //LOG(DEBUG) << "Retry Request body: " << utility::conversions::to_utf8string(req.to_string());

        raw_response = hr.get();
        lck.unlock();

      } // PERFORMANCE BRACE

      try
      {
        LOG(DEBUG) << "Final response...";

        Response* response = new Response;
        response->setResponseCode((ResponseCode)raw_response.status_code());
        HttpHeaders h;
        AuthenticatingProxy::copyHeaders(raw_response.headers(),h);
        response->setResponseHeaders(h); // also sets response type via Content-type header

        std::vector<unsigned char> vec = raw_response.extract_vector().get();
        std::string str;
        str.reserve(vec.size());
        str.assign(vec.begin(),vec.end());
        response->setContent(str);
        /*
        if (response->getResponseType() == ResponseType::BINARY) {
          LOG(DEBUG) << "AuthenticatingProxy - Got binary response";
          std::vector<unsigned char> vec = raw_response.extract_vector().get();
          // convert to binary content, or store for now...
          std::string* str = new std::string;
          str->reserve(vec.size());
          str->assign(vec.begin(),vec.end());
          response->setContent(str);
        } else {
          LOG(DEBUG) << "AuthenticatingProxy - Got text response";
          response->setContent(new std::string(utility::conversions::to_utf8string(raw_response.extract_string().get())));
        }
        */

        LOG(DEBUG) << response->getContent();

        return response;
      }
      catch (const web::http::http_exception& e)
      {
        // Print error.
        LOG(DEBUG) << "Error authenticating on second attempt: " << e.what();
        //std::wostringstream ss;
        //ss << "There was an error!!!!" << e.what() << std::endl;
        //std::wcout << ss.str();
      }

    } catch(std::exception &e) {
      LOG(DEBUG) << "Overall exception: " << e.what();
      //std::cerr << e.what() << std::endl;
    }
  }

  return nullptr; // return the same inout parameter to indicate more obviously that we change it
}

Response* AuthenticatingProxy::getSync(const std::string& host,
    const std::string& path,
    const mlclient::HttpHeaders& headers)
{
  TIMED_FUNC(AuthenticatingProxy_getSync);
  Response* response = this->doRequest(utility::conversions::to_utf8string(http::methods::GET),host,path,headers,nullptr);

  return response;
}


Response* AuthenticatingProxy::postSync(const std::string& host,
    const std::string& path,
    const IDocumentContent& body,
    const mlclient::HttpHeaders& headers)
{
  TIMED_FUNC(AuthenticatingProxy_postSync);
  LOG(DEBUG) << "    Entering postSync";
  LOG(DEBUG) << "    Post content: " << body.getContent();
  Response* response = doRequest(utility::conversions::to_utf8string(http::methods::POST),host,path,headers,&body);
  LOG(DEBUG) << "    Response content: " << response->getContent();
  LOG(DEBUG) << "    Leaving postSync";

  return response;
}

void AuthenticatingProxy::buildBulkPayload(const DocumentSet& set,const long startIdx,const long endIdx, std::ostringstream& sout) {
  //for (list<Document>::iterator it=set.begin(); it!=set.end(); ++it) {
  for (long i = startIdx;i <= endIdx;i++) {
    const Document& it = set.at(i);
    sout << "--BOUNDARY\r\n";

    // send properties, collections and permissions too

    GenericTextDocumentContent* tdc = new GenericTextDocumentContent;
    std::ostringstream pos;
    // TODO specify MIME type based on MIME type of properties document (could be JSON or XML)
    pos << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    pos << "<rapi:metadata xmlns:rapi=\"http://marklogic.com/rest-api\">";
    pos << "  <rapi:quality>1</rapi:quality>";
    pos << "  <prop:properties xmlns:prop=\"http://marklogic.com/xdmp/property\">";
    // specify properties here
    //pos << "    <my-prop>my first property</my-prop>";
    pos << "  </prop:properties>";
    pos << "  <rapi:collections>";
    const std::vector<std::string> cols = it.getCollections();
    for (auto colsIter = cols.begin(); colsIter != cols.end();++colsIter) {
      pos << "    <rapi:collection>" << *colsIter << "</rapi:collection>";
    }
    pos << "  </rapi:collections>";
    pos << "  <rapi:permissions>";
    const std::vector<Permission> perms = it.getPermissions();
    for (auto permIter = perms.begin(); permIter != perms.end();++permIter) {
      pos << "    <rapi:permission>";
      pos << "      <rapi:role-name>" << permIter->getRole() << "</rapi:role-name>";
      pos << "      <rapi:capability>" << permIter->getCapability() << "</rapi:capability>";
      pos << "    </rapi:permission>";
    }
    pos << "  </rapi:permissions>";
    pos << "</rapi:metadata>";
    tdc->setContent(pos.str());
    tdc->setMimeType(mlclient::IDocumentContent::MIME_XML);

    // metadata FIRST
    sout << "Content-Type: " << tdc->getMimeType() << "\r\n";
    sout << "Content-Disposition: attachment; filename=\"" << it.getUri() << "\"; category=metadata\r\n";
    sout << "Content-Length: " << tdc->getLength() << "\r\n";
    sout << "\r\n";
    sout << tdc->getContent();
    sout << "\r\n";

    sout << "--BOUNDARY\r\n";

    const IDocumentContent* idc = it.getContent();
    std::string content = idc->getContent(); // TODO support binary objects

    sout << "Content-Type: " << idc->getMimeType() << "\r\n";
    sout << "Content-Disposition: attachment;filename=\"" << it.getUri() << "\"\r\n";
    sout << "Content-Length: " << content.size() << "\r\n";

    sout << "\r\n";

    sout << content << "\r\n";
  }

  sout << "--BOUNDARY--\r\n" << std::endl;

  //cout << "DUMP BULK PAYLOAD - START" << endl;
  //cout << bulkPayload << endl;
  //cout << "DUMP BULK PAYLOAD - END" << endl;

}

Response* AuthenticatingProxy::multiPostSync(const std::string& host,const std::string& path,
    const DocumentSet& allContent, const long startPosInclusive,
    const long endPosInclusive, const mlclient::HttpHeaders& commonHeaders) {
  TIMED_FUNC(AuthenticatingProxy_multiPostSync);
  LOG(DEBUG) << "    Entering multiPostSync";

  GenericTextDocumentContent body;
  body.setMimeType("multipart/mixed");
  std::ostringstream content;
  buildBulkPayload(allContent,startPosInclusive,endPosInclusive,content);
  std::string ct = content.str();
  body.setContent(ct);

  HttpHeaders headers = commonHeaders; // copy assignment operator
  headers.setHeader("Content-type","multipart/mixed; boundary=BOUNDARY");
  headers.setHeader("Accept",IDocumentContent::MIME_JSON);
  std::ostringstream os;
  os << ct.length();
  headers.setHeader("Content-Length",os.str());

  //LOG(DEBUG) << "    Multi Post content: " << body.getContent();
  Response* response = doRequest(utility::conversions::to_utf8string(http::methods::POST),host,path,headers,&body);
  LOG(DEBUG) << "    Leaving multiPostSync";

  return response;
}

Response* AuthenticatingProxy::putSync(const std::string& host,
    const std::string& path,
    const IDocumentContent& body,
    const mlclient::HttpHeaders& headers)
{
  TIMED_FUNC(AuthenticatingProxy_putSync);
  Response* response = doRequest(utility::conversions::to_utf8string(http::methods::PUT),host,path,headers,&body);

  return response;
}

Response* AuthenticatingProxy::deleteSync(const std::string& host,
    const std::string& path,
    const mlclient::HttpHeaders& headers)
{
  TIMED_FUNC(AuthenticatingProxy_deleteSync);
  Response* response = doRequest(utility::conversions::to_utf8string(http::methods::DEL),host,path,headers,nullptr);

  return response;
}

} // end internals namespace

} // end mlclient namespace

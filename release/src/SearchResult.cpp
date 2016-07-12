/*
 * SearchResult.cpp
 *
 *  Created on: 26 May 2016
 *      Author: adamfowler
 */

#include "mlclient/SearchResult.hpp"
#include "mlclient/DocumentContent.hpp"
#include "mlclient/ext/easylogging++.h"

namespace mlclient {

const std::string SearchResult::JSON = "json";
const std::string SearchResult::XML = "xml";

SearchResult::SearchResult() : index(0),uri(""),path(""),score(0),confidence(0.0),fitness(0.0),detail(DETAIL::NONE),detailContent(nullptr) {
  TIMED_FUNC(SearchResult_defaultConstructor);
  LOG(DEBUG) << "    SearchResult::defaultConstructor @" << &*this;
}

SearchResult::~SearchResult() {
  TIMED_FUNC(SearchResult_destructor);
  LOG(DEBUG) << "    SearchResult::destructor @" << &*this;
  //delete this->detailContent;
  //detailContent = NULL;
}

SearchResult::SearchResult(const long index, const std::string& uri, const std::string& path,const long score,
    const double confidence,const double fitness,const DETAIL& detail,IDocumentContent* own_detailContent,
    const std::string& mimeType,const std::string& format) {
  TIMED_FUNC(SearchResult_detailConstructor);
  LOG(DEBUG) << "    SearchResult::detailedConstructor @" << &*this;
  this->index = index;
  this->uri = uri;
  this->path = path;
  this->score = score;
  this->confidence = confidence;
  this->fitness = fitness;
  this->detail = detail;
  this->detailContent = own_detailContent;
  this->mimeType = mimeType;
  this->format = format;
}

SearchResult::SearchResult(SearchResult&& other) {
  TIMED_FUNC(SearchResult_moveConstructor);
  LOG(DEBUG) << "    SearchResult::moveConstructor @" << &*this;
  this->index = std::move(other.index);
  this->uri = std::move(other.uri);
  this->path = std::move(other.path);
  this->score = std::move(other.score);
  this->fitness = std::move(other.fitness);
  this->detail = std::move(other.detail);
  this->detailContent = other.detailContent;
  other.detailContent = NULL;
  this->mimeType = std::move(other.mimeType);
  this->format = std::move(other.format);
  this->confidence = std::move(other.confidence);
}

long SearchResult::getIndex() {
  return index;
}
const std::string& SearchResult::getUri() const {
  return uri;
}
const std::string& SearchResult::getPath() const {
  return path;
}
long SearchResult::getScore() {
  return score;
}
double SearchResult::getConfidence() {
  return confidence;
}
double SearchResult::getFitness() {
  return fitness;
}
const SearchResult::DETAIL& SearchResult::getDetail() const {
  return detail;
}
const IDocumentContent* SearchResult::getDetailContent() const {
  TIMED_FUNC(SearchResult_getDetailContent);
  return detailContent;
}
const std::string& SearchResult::getMimeType() const {
  return mimeType;
}
const std::string& SearchResult::getFormat() const {
  return format;
}

} // end namespace mlclient
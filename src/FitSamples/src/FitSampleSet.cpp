//
// Created by Nadrino on 22/07/2021.
//

#include <TTreeFormulaManager.h>
#include "json.hpp"

#include "Logger.h"
#include "GenericToolbox.h"

#include "JsonUtils.h"
#include "GlobalVariables.h"
#include "FitSampleSet.h"


LoggerInit([](){
  Logger::setUserHeaderStr("[FitSampleSet]");
})

FitSampleSet::FitSampleSet() { this->reset(); }
FitSampleSet::~FitSampleSet() { this->reset(); }

void FitSampleSet::reset() {
  _isInitialized_ = false;
  _config_.clear();
//  _statFluctuation_ = false;
  
  _likelihoodFunctionPtr_ = nullptr;
  _fitSampleList_.clear();
  _dataEventType_ = DataEventType::Unset;

  _eventByEventDialLeafList_.clear();
}

void FitSampleSet::setConfig(const nlohmann::json &config) {
  _config_ = config;
  while( _config_.is_string() ){
    LogWarning << "Forwarding " << __CLASS_NAME__ << " config: \"" << _config_.get<std::string>() << "\"" << std::endl;
    _config_ = JsonUtils::readConfigFile(_config_.get<std::string>());
  }
}

void FitSampleSet::addEventByEventDialLeafName(const std::string& leafName_){
  if( not GenericToolbox::doesElementIsInVector(leafName_, _eventByEventDialLeafList_) ){
    _eventByEventDialLeafList_.emplace_back(leafName_);
  }
}

void FitSampleSet::initialize() {
  LogWarning << __METHOD_NAME__ << std::endl;

  LogAssert(not _config_.empty(), "_config_ is not set." << std::endl);

  _dataEventType_ = DataEventTypeEnumNamespace::toEnum(JsonUtils::fetchValue<std::string>(_config_, "dataEventType"), true );
  LogInfo << "Data events type is set to: " << DataEventTypeEnumNamespace::toString(_dataEventType_) << std::endl;
  _statFluctuation_ = JsonUtils::fetchValue(_config_, "statFluctuation", true);
  LogDebug<< "Statistical fluctuations on data are : "<<(_statFluctuation_ == false ?  "OFF" : "ON")<<std::endl;
  
  LogInfo << "Reading samples definition..." << std::endl;

  bool _isEnabled_{false};
  auto fitSampleListConfig = JsonUtils::fetchValue(_config_, "fitSampleList", nlohmann::json());
  for( const auto& fitSampleConfig: fitSampleListConfig ){ 
    _isEnabled_ = JsonUtils::fetchValue(fitSampleConfig, "isEnabled", true);
    if( not _isEnabled_ ) {
       std::string _Name_ = JsonUtils::fetchValue<std::string>(fitSampleConfig, "name");
       LogDebug << "Sample "<<_Name_<<" is disabled" << std::endl;
       continue;}
    _fitSampleList_.emplace_back();
    _fitSampleList_.back().setConfig(fitSampleConfig);
    _fitSampleList_.back().initialize();
  }



  LogInfo << "Creating parallelisable jobs" << std::endl;

  // Fill the bin index inside of each event
  std::function<void(int)> updateSampleEventBinIndexesFct = [this](int iThread){
    for( auto& sample : _fitSampleList_ ){
      sample.getMcContainer().updateEventBinIndexes(iThread);
      sample.getDataContainer().updateEventBinIndexes(iThread);
    }
  };
  GlobalVariables::getParallelWorker().addJob("FitSampleSet::updateSampleEventBinIndexes", updateSampleEventBinIndexesFct);

  // Fill bin event caches
  std::function<void(int)> updateSampleBinEventListFct = [this](int iThread){
    for( auto& sample : _fitSampleList_ ){
      
      sample.getMcContainer().updateBinEventList(iThread);
      sample.getDataContainer().updateBinEventList(iThread);
    }
  };
  GlobalVariables::getParallelWorker().addJob("FitSampleSet::updateSampleBinEventList", updateSampleBinEventListFct);


  // Histogram fills
  std::function<void(int)> refillMcHistogramsFct = [this](int iThread){
    for( auto& sample : _fitSampleList_ ){
      
      sample.getMcContainer().refillHistogram(iThread);
      sample.getDataContainer().refillHistogram(iThread);
    }
  };
  std::function<void()> rescaleMcHistogramsFct = [this](){
    for( auto& sample : _fitSampleList_ ){
      
      sample.getMcContainer().rescaleHistogram();
      sample.getDataContainer().rescaleHistogram();
    }
  };
  GlobalVariables::getParallelWorker().addJob("FitSampleSet::updateSampleHistograms", refillMcHistogramsFct);
  GlobalVariables::getParallelWorker().setPostParallelJob("FitSampleSet::updateSampleHistograms", rescaleMcHistogramsFct);

  _likelihoodFunctionPtr_ = std::shared_ptr<PoissonLLH>(new PoissonLLH);

  _isInitialized_ = true;
}

DataEventType FitSampleSet::getDataEventType() const {
  return _dataEventType_;
}
const std::vector<FitSample> &FitSampleSet::getFitSampleList() const {
  return _fitSampleList_;
}
std::vector<FitSample> &FitSampleSet::getFitSampleList() {
  return _fitSampleList_;
}
const nlohmann::json &FitSampleSet::getConfig() const {
  return _config_;
}

bool FitSampleSet::empty() const {
  return _fitSampleList_.empty();
}
double FitSampleSet::evalLikelihood() const{
  LogDebug<< "SONO QUIIIIIIII "<<std::endl;
  double llh = 0.;
  for( auto& sample : _fitSampleList_ ){
    
    double sampleLlh = 0;
    for( int iBin = 1 ; iBin <= sample.getMcContainer().histogram->GetNbinsX() ; iBin++ ){
      sampleLlh += (*_likelihoodFunctionPtr_)(
        sample.getMcContainer().histogram->GetBinContent(iBin),
        sample.getMcContainer().histogram->GetBinError(iBin),
        sample.getDataContainer().histogram->GetBinContent(iBin));
    }
    llh += sampleLlh;
  }

  return llh;
}



void FitSampleSet::loadAsimovData(){
  if( _dataEventType_ == DataEventType::Asimov ){
    LogWarning << "Asimov data selected: copying MC events..." << std::endl;
    for( auto& sample : _fitSampleList_ ){      
      
      

      LogInfo << "Copying MC events in sample \"" << sample.getName() << "\"" << std::endl;

      sample.getDataContainer().eventList = sample.getMcContainer().eventList;

//      auto& dataEventList = sample.getDataContainer().eventList;
//      LogThrowIf(not dataEventList.empty(), "Can't fill Asimov data, dataEventList is not empty.");
//      auto& mcEventList = sample.getMcContainer().eventList;
//      dataEventList.resize(mcEventList.size());
//      for( size_t iEvent = 0 ; iEvent < dataEventList.size() ; iEvent++ ){
//        dataEventList[iEvent] = mcEventList[iEvent];
//      }
    }
  }
}

void FitSampleSet::applyStatFluctOnData(){

 if(_statFluctuation_){
     LogDebug<< "APPLY STAT FLUCT ON DATA "<<std::endl;
     for( auto& sample : _fitSampleList_ ){
      
       for( int iBin = 1 ; iBin <= sample.getDataContainer().histogram->GetNbinsX() ; iBin++ ){
  	 
  	 double bincont = sample.getDataContainer().histogram->GetBinContent(iBin);
  	 double val = gRandom->Poisson(bincont);
  	 sample.getDataContainer().histogram->SetBinContent(iBin,val);
  	 //if (iBin%50==0) LogDebug<< iBin << " " <<bincont<<" " << val<< std::endl;
	 //if (iBin==1 || iBin==10)sample.getDataContainer().histogram->SetBinContent(iBin,1000);
       }
       
    }
  }
}

void FitSampleSet::updateSampleEventBinIndexes() const{
  if( _showTimeStats_ ) GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("FitSampleSet::updateSampleEventBinIndexes");
  if( _showTimeStats_ ) LogDebug << __METHOD_NAME__ << " took: " << GenericToolbox::getElapsedTimeSinceLastCallStr(__METHOD_NAME__) << std::endl;
}
void FitSampleSet::updateSampleBinEventList() const{
  if( _showTimeStats_ ) GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("FitSampleSet::updateSampleBinEventList");
  if( _showTimeStats_ ) LogDebug << __METHOD_NAME__ << " took: " << GenericToolbox::getElapsedTimeSinceLastCallStr(__METHOD_NAME__) << std::endl;
}
void FitSampleSet::updateSampleHistograms() const {
  if( _showTimeStats_ ) GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("FitSampleSet::updateSampleHistograms");
  if( _showTimeStats_ ) LogDebug << __METHOD_NAME__ << " took: " << GenericToolbox::getElapsedTimeSinceLastCallStr(__METHOD_NAME__) << std::endl;
}



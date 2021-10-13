//
// Created by Nadrino on 11/06/2021.
//

#include <AnaTreeMC.hh>
#include "vector"

#include "GenericToolbox.h"
#include "GenericToolbox.Root.h"

#include "JsonUtils.h"
#include "Propagator.h"
#include "GlobalVariables.h"
#include "Dial.h"
#include "FitParameterSet.h"

#include "NormalizationDial.h"
#include "SplineDial.h"

LoggerInit([](){
  Logger::setUserHeaderStr("[Propagator]");
})

Propagator::Propagator() { this->reset(); }
Propagator::~Propagator() { this->reset(); }

void Propagator::reset() {
  _isInitialized_ = false;
  _parameterSetsList_.clear();
  _saveDir_ = nullptr;

  std::vector<std::string> jobNameRemoveList;
  for( const auto& jobName : GlobalVariables::getParallelWorker().getJobNameList() ){
    if(jobName == "Propagator::fillEventDialCaches"
    or jobName == "Propagator::reweightSampleEvents"
    or jobName == "Propagator::refillSampleHistograms"
    or jobName == "Propagator::applyResponseFunctions"
      ){
      jobNameRemoveList.emplace_back(jobName);
    }
  }
  for( const auto& jobName : jobNameRemoveList ){
    GlobalVariables::getParallelWorker().removeJob(jobName);
  }

  _responseFunctionsSamplesMcHistogram_.clear();
  _nominalSamplesMcHistogram_.clear();
}

void Propagator::setShowTimeStats(bool showTimeStats) {
  _showTimeStats_ = showTimeStats;
}
void Propagator::setSaveDir(TDirectory *saveDir) {
  _saveDir_ = saveDir;
}
void Propagator::setConfig(const json &config) {
  _config_ = config;
  while( _config_.is_string() ){
    LogWarning << "Forwarding " << __CLASS_NAME__ << " config: \"" << _config_.get<std::string>() << "\"" << std::endl;
    _config_ = JsonUtils::readConfigFile(_config_.get<std::string>());
  }
}

void Propagator::initialize() {
  LogWarning << __METHOD_NAME__ << std::endl;

  LogTrace << "Parameters..." << std::endl;
  auto parameterSetListConfig = JsonUtils::fetchValue<json>(_config_, "parameterSetListConfig");
  if( parameterSetListConfig.is_string() ) parameterSetListConfig = JsonUtils::readConfigFile(parameterSetListConfig.get<std::string>());
  int nPars = 0;
  _parameterSetsList_.reserve(parameterSetListConfig.size()); // make sure the objects aren't moved in RAM ( since FitParameter* will be used )
  for( const auto& parameterSetConfig : parameterSetListConfig ){
    _parameterSetsList_.emplace_back();
    _parameterSetsList_.back().setJsonConfig(parameterSetConfig);
    _parameterSetsList_.back().setSaveDir(GenericToolbox::mkdirTFile(_saveDir_, "ParameterSets"));
    _parameterSetsList_.back().initialize();
    nPars += _parameterSetsList_.back().getNbParameters();
    LogInfo << _parameterSetsList_.back().getSummary() << std::endl;
  }

  _globalCovarianceMatrix_ = std::shared_ptr<TMatrixD>( new TMatrixD(nPars, nPars) );
  int iParOffset = 0;
  for( const auto& parSet : _parameterSetsList_ ){
    if( not parSet.isEnabled() ) continue;
    for( int iCov = 0 ; iCov < parSet.getOriginalCovarianceMatrix()->GetNrows() ; iCov++ ){
      for( int jCov = 0 ; jCov < parSet.getOriginalCovarianceMatrix()->GetNcols() ; jCov++ ){
        (*_globalCovarianceMatrix_)[iParOffset+iCov][iParOffset+jCov] = (*parSet.getOriginalCovarianceMatrix())[iCov][jCov];
      }
    }
    iParOffset += parSet.getOriginalCovarianceMatrix()->GetNrows();
  }
  if( _saveDir_ != nullptr ){
    _saveDir_->cd();
    _globalCovarianceMatrix_->Write("globalCovarianceMatrix_TMatrixD");
  }

  LogDebug << "FitSampleSet..." << std::endl;
  auto fitSampleSetConfig = JsonUtils::fetchValue(_config_, "fitSampleSetConfig", nlohmann::json());
  _fitSampleSet_.setConfig(fitSampleSetConfig);
  _fitSampleSet_.initialize();


  LogTrace << "Initializing the PlotGenerator" << std::endl;
  auto plotGeneratorConfig = JsonUtils::fetchValue<json>(_config_, "plotGeneratorConfig");
  if( plotGeneratorConfig.is_string() ) parameterSetListConfig = JsonUtils::readConfigFile(plotGeneratorConfig.get<std::string>());
  _plotGenerator_.setConfig(plotGeneratorConfig);
  _plotGenerator_.initialize();

  LogInfo << "Polling the requested leaves to load in memory..." << std::endl;
  for( auto& dataSet : _fitSampleSet_.getDataSetList() ){

    // parSet
    for( auto& parSet : _parameterSetsList_ ){
      if( not parSet.isEnabled() ) continue;

      for( auto& par : parSet.getParameterList() ){
        if( not par.isEnabled() ) continue;

        auto* dialSetPtr = par.findDialSet( dataSet.getName() );
        if( dialSetPtr == nullptr ){ continue; }

        if( dialSetPtr->getApplyConditionFormula() != nullptr ){
          for( int iPar = 0 ; iPar < dialSetPtr->getApplyConditionFormula()->GetNpar() ; iPar++ ){
            dataSet.addRequestedLeafName(dialSetPtr->getApplyConditionFormula()->GetParName(iPar));
          }
        }

        for( auto& dial : dialSetPtr->getDialList() ){
          for( auto& var : dial->getApplyConditionBin().getVariableNameList() ){
            dataSet.addRequestedLeafName(var);
          } // var
        } // dial
      } // par
    } // parSet

    // plotGen
    auto varListRequestedByPlotGen = _plotGenerator_.fetchRequestedLeafNames();
    for( const auto& varName : varListRequestedByPlotGen ){
      dataSet.addRequestedLeafName(varName);
    }

  } // dataSets

  _fitSampleSet_.loadPhysicsEvents();
  _plotGenerator_.setFitSampleSetPtr(&_fitSampleSet_);
  _plotGenerator_.defineHistogramHolders();

  initializeThreads();
  initializeCaches();

  fillEventDialCaches();

  // TEMP: trim dial cache
  LogDebug << "Trimming event dial cache..." << std::endl;
  for( auto& sample: _fitSampleSet_.getFitSampleList() ){
    for( auto& event : sample.getMcContainer().eventList ){
      event.trimDialCache();
    }
  }


  if( JsonUtils::fetchValue<json>(_config_, "throwParameters", false) ){
    LogWarning << "Throwing parameters..." << std::endl;
    for( auto& parSet : _parameterSetsList_ ){
      auto thrownPars = GenericToolbox::throwCorrelatedParameters(GenericToolbox::getCholeskyMatrix(
        parSet.getOriginalCovarianceMatrix()));
      for( auto& par : parSet.getParameterList() ){
        par.setParameterValue( par.getPriorValue() + thrownPars.at(par.getParameterIndex()) );
        LogDebug << parSet.getName() << "/" << par.getTitle() << ": thrown = " << par.getParameterValue() << std::endl;
      }
    }
  }

  LogInfo << "Propagating prior parameters on events..." << std::endl;
  reweightSampleEvents();

  LogInfo << "Set the current MC prior weights as nominal weight..." << std::endl;
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    for( auto& event : sample.getMcContainer().eventList ){
      event.setNominalWeight(event.getEventWeight());
    }
  }

  if( _fitSampleSet_.getDataEventType() == DataEventType::Asimov ){
    LogInfo << "Propagating prior weights on data Asimov events..." << std::endl;
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      sample.getDataContainer().histScale = sample.getMcContainer().histScale;
      int nEvents = int(sample.getMcContainer().eventList.size());
      for( int iEvent = 0 ; iEvent < nEvents ; iEvent++ ){
        // Since no reweight is applied on data samples, the nominal weight should be the default one
        sample.getDataContainer().eventList.at(iEvent).setTreeWeight(
          sample.getMcContainer().eventList.at(iEvent).getNominalWeight()
        );
        sample.getDataContainer().eventList.at(iEvent).resetEventWeight();
        sample.getDataContainer().eventList.at(iEvent).setNominalWeight(sample.getDataContainer().eventList.at(iEvent).getEventWeight());
      }
    }
  }

  LogInfo << "Filling up sample bin caches..." << std::endl;
  _fitSampleSet_.updateSampleBinEventList();

  LogInfo << "Filling up sample histograms..." << std::endl;
  _fitSampleSet_.updateSampleHistograms();

  // Now the data won't be refilled each time
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    sample.getDataContainer().isLocked = true;
  }

  _useResponseFunctions_ = JsonUtils::fetchValue<json>(_config_, "DEV_useResponseFunctions", false);
  if( _useResponseFunctions_ ){ this->makeResponseFunctions(); }

  if( JsonUtils::fetchValue<json>(_config_, "throwParameters", false) ){
    for( auto& parSet : _parameterSetsList_ ){
      for( auto& par : parSet.getParameterList() ){
        par.setParameterValue( par.getPriorValue() );
      }
    }
  }

  _isInitialized_ = true;
}

bool Propagator::isUseResponseFunctions() const {
  return _useResponseFunctions_;
}
FitSampleSet &Propagator::getFitSampleSet() {
  return _fitSampleSet_;
}
std::vector<FitParameterSet> &Propagator::getParameterSetsList() {
  return _parameterSetsList_;
}
PlotGenerator &Propagator::getPlotGenerator() {
  return _plotGenerator_;
}
const json &Propagator::getConfig() const {
  return _config_;
}


void Propagator::propagateParametersOnSamples(){

  if(not _useResponseFunctions_ or not _isRfPropagationEnabled_ ){
    reweightSampleEvents();
    refillSampleHistograms();
  }
  else{
    applyResponseFunctions();
  }

}
void Propagator::reweightSampleEvents() {
  GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("Propagator::reweightSampleEvents");
  weightProp.counts++; weightProp.cumulated += GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
}
void Propagator::refillSampleHistograms(){
  GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("Propagator::refillSampleHistograms");
  fillProp.counts++; fillProp.cumulated += GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
}
void Propagator::applyResponseFunctions(){
  GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("Propagator::applyResponseFunctions");
  applyRf.counts++; applyRf.cumulated += GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
}

void Propagator::preventRfPropagation(){
  if(_isRfPropagationEnabled_){
//    LogInfo << "Parameters propagation using Response Function is now disabled." << std::endl;
    _isRfPropagationEnabled_ = false;
  }
}
void Propagator::allowRfPropagation(){
  if(not _isRfPropagationEnabled_){
//    LogWarning << "Parameters propagation using Response Function is now ENABLED." << std::endl;
    _isRfPropagationEnabled_ = true;
  }
}


// Protected
void Propagator::initializeThreads() {

  std::function<void(int)> fillEventDialCacheFct = [this](int iThread){
    this->fillEventDialCaches(iThread);
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::fillEventDialCaches", fillEventDialCacheFct);

  std::function<void(int)> reweightSampleEventsFct = [this](int iThread){
    this->reweightSampleEvents(iThread);
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::reweightSampleEvents", reweightSampleEventsFct);

  std::function<void(int)> refillSampleHistogramsFct = [this](int iThread){
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      sample.getMcContainer().refillHistogram(iThread);
      sample.getDataContainer().refillHistogram(iThread);
    }
  };
  std::function<void()> refillSampleHistogramsPostParallelFct = [this](){
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      sample.getMcContainer().rescaleHistogram();
      sample.getDataContainer().rescaleHistogram();
    }
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::refillSampleHistograms", refillSampleHistogramsFct);
  GlobalVariables::getParallelWorker().setPostParallelJob("Propagator::refillSampleHistograms", refillSampleHistogramsPostParallelFct);

  std::function<void(int)> applyResponseFunctionsFct = [this](int iThread){
    this->applyResponseFunctions(iThread);
  };
  GlobalVariables::getParallelWorker().addJob("Propagator::applyResponseFunctions", applyResponseFunctionsFct);

}
void Propagator::initializeCaches() {
  LogInfo << __METHOD_NAME__ << std::endl;

  size_t preSize;
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    for( auto& event : sample.getMcContainer().eventList ){

      preSize = 0;
      for( auto& parSet : _parameterSetsList_ ){
        if( parSet.isUseOnlyOneParameterPerEvent() ){ preSize += 1; }
        else{ preSize += parSet.getNbParameters(); }
      }
      event.getRawDialPtrList().resize(preSize);

    } // event
  } // sample

}
void Propagator::fillEventDialCaches(){
  LogInfo << __METHOD_NAME__ << std::endl;
  GlobalVariables::getParallelWorker().runJob("Propagator::fillEventDialCaches");
}

void Propagator::makeResponseFunctions(){
  LogWarning << __METHOD_NAME__ << std::endl;

  this->preventRfPropagation(); // make sure, not yet setup

  for( auto& parSet : _parameterSetsList_ ){
    for( auto& par : parSet.getParameterList() ){
      par.setParameterValue(par.getPriorValue());
    }
  }
  this->propagateParametersOnSamples();

  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    _nominalSamplesMcHistogram_[&sample] = std::shared_ptr<TH1D>((TH1D*) sample.getMcContainer().histogram->Clone());
  }

  for( auto& parSet : _parameterSetsList_ ){
    for( auto& par : parSet.getParameterList() ){
      LogInfo << "Make RF for " << parSet.getName() << "/" << par.getTitle() << std::endl;
      par.setParameterValue(par.getPriorValue() + par.getStdDevValue());

      this->propagateParametersOnSamples();

      for( auto& sample : _fitSampleSet_.getFitSampleList() ){
        _responseFunctionsSamplesMcHistogram_[&sample].emplace_back(std::shared_ptr<TH1D>((TH1D*) sample.getMcContainer().histogram->Clone()) );
        GenericToolbox::transformBinContent(_responseFunctionsSamplesMcHistogram_[&sample].back().get(), [&](TH1D* h_, int b_){
          h_->SetBinContent(
            b_,
            (h_->GetBinContent(b_)/_nominalSamplesMcHistogram_[&sample]->GetBinContent(b_))-1);
          h_->SetBinError(b_,0);
        });
      }

      par.setParameterValue(par.getPriorValue());
    }
  }
  this->propagateParametersOnSamples(); // back to nominal

  // WRITE
  if( _saveDir_ != nullptr ){
    auto* rfDir = GenericToolbox::mkdirTFile(_saveDir_, "RF");
    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      GenericToolbox::mkdirTFile(rfDir, "nominal")->cd();
      _nominalSamplesMcHistogram_[&sample]->Write(Form("nominal_%s", sample.getName().c_str()));

      int iPar = -1;
      auto* devDir = GenericToolbox::mkdirTFile(rfDir, "deviation");
      for( auto& parSet : _parameterSetsList_ ){
        auto* parSetDir = GenericToolbox::mkdirTFile(devDir, parSet.getName());
        for( auto& par : parSet.getParameterList() ){
          iPar++;
          GenericToolbox::mkdirTFile(parSetDir, par.getTitle())->cd();
          _responseFunctionsSamplesMcHistogram_[&sample].at(iPar)->Write(Form("dev_%s", sample.getName().c_str()));
        }
      }
    }
    _saveDir_->cd();
  }

  LogInfo << "RF built" << std::endl;
}

void Propagator::reweightSampleEvents(int iThread_) {
  int nThreads = GlobalVariables::getNbThreads();
  if(iThread_ == -1){
    // force single thread
    nThreads = 1;
    iThread_ = 0;
  }

  //! Warning: everything you modify here, may significantly slow down the fitter

  // This loop is slightly faster that the next one (~1% faster)
  // Memory needed: 2*32bits(int) + 64bits(ptr)
  // 3 write per sample
  // 1 write per event
  int iEvent;
  int nEvents;
  std::vector<PhysicsEvent>* evList{nullptr};
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    evList = &sample.getMcContainer().eventList;
    iEvent = iThread_;
    nEvents = int(evList->size());
    while( iEvent < nEvents ){
      (*evList)[iEvent].reweightUsingDialCache();
      iEvent += nThreads;
    }
  }

//  // Slower loop
//  // Memory: 2*64bits
//  // per sample: 1 read, 2 writes (each requires to fetch the event array multiple times)
//  // 1 write per event
//  PhysicsEvent* evPtr{nullptr};
//  PhysicsEvent* evLastPtr{nullptr};
//  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
//    if( sample.getMcContainer().eventList.empty() ) continue;
//    evPtr = &sample.getMcContainer().eventList[iThread_];
//    evLastPtr = &sample.getMcContainer().eventList.back();
//
//    while( evPtr <= evLastPtr ){
//      evPtr->reweightUsingDialCache();
//      evPtr += nThreads;
//    }
//  }

}
void Propagator::fillEventDialCaches(int iThread_){

  DialSet* parameterDialSetPtr;
  PhysicsEvent* evPtr{nullptr};
  std::vector<Dial *>* evParSetDialList{nullptr};
  auto& dataSetList = _fitSampleSet_.getDataSetList();
  size_t iEvent, nEvents;
  size_t iDialSet, iDial;
  size_t iVar;
  size_t eventDialOffset;
  DialSet* dialSetPtr;
  const DataBin* applyConditionBinPtr;
  for( size_t iDataSet = 0 ; iDataSet < dataSetList.size() ; iDataSet++ ){

    std::map<FitParameterSet*, std::vector<DialSet*>> dialSetPtrMap;
    for( auto& parSet : _parameterSetsList_ ){
      if( not parSet.isEnabled() ){ continue; }
      for( auto& par : parSet.getParameterList() ){
        if( not par.isEnabled() ){ continue; }
        dialSetPtr = par.findDialSet( dataSetList.at(iDataSet).getName());
        if( dialSetPtr != nullptr and not dialSetPtr->getDialList().empty() ){
          dialSetPtrMap[&parSet].emplace_back( dialSetPtr );
        }
      }
    }

    for( auto& sample : _fitSampleSet_.getFitSampleList() ){
      if( not GenericToolbox::doesElementIsInVector(iDataSet, sample.getMcContainer().dataSetIndexList) ){ continue; }

      std::stringstream ss;
      ss << LogWarning.getPrefixString() << sample.getName();

      nEvents = sample.getMcContainer().eventNbList[iDataSet];
      for( iEvent = sample.getMcContainer().eventOffSetList[iDataSet] ; iEvent < nEvents ; iEvent++ ){
        if (iEvent % GlobalVariables::getNbThreads() != iThread_) {
          continue;
        }
        if (iThread_ == GlobalVariables::getNbThreads() - 1) {
          GenericToolbox::displayProgressBar(iEvent, nEvents, ss.str());
        }

        evPtr = &sample.getMcContainer().eventList[iEvent];

        eventDialOffset = 0;
        for( auto& dialSetPair : dialSetPtrMap ){
          for( iDialSet = 0 ; iDialSet < dialSetPair.second.size() ; iDialSet++ ){
            dialSetPtr = dialSetPair.second[iDialSet];

            if( dialSetPtr->getApplyConditionFormula() != nullptr ){
              if( evPtr->evalFormula(dialSetPtr->getApplyConditionFormula()) == 0 ){
                continue;
              }
            }

            bool isInBin = false;
            for( iDial = 0 ; iDial < dialSetPtr->getDialList().size(); iDial++ ){
              applyConditionBinPtr = &dialSetPtr->getDialList()[iDial]->getApplyConditionBin();
              isInBin = true;

              for( iVar = 0 ; iVar < applyConditionBinPtr->getVariableNameList().size() ; iVar++ ){
                if( not applyConditionBinPtr->isBetweenEdges(iVar, evPtr->getVarAsDouble(applyConditionBinPtr->getVariableNameList()[iVar] ) )){
                  isInBin = false;
                  break;
                }
              }
              if( isInBin ){
                evPtr->getRawDialPtrList()[eventDialOffset++] = dialSetPtr->getDialList()[iDial].get();
                break;
              }
            } // iDial

            if( isInBin and dialSetPair.first->isUseOnlyOneParameterPerEvent() ){
              break; // leave iDialSet / enabled parameters loop
            }

          } // iDialSet / Enabled-parameter
        } // ParSet / DialSet Pairs
      } // iEvent
    } // iSample
  } // iDataSet

}
void Propagator::applyResponseFunctions(int iThread_){

  TH1D* histBuffer{nullptr};
  TH1D* nominalHistBuffer{nullptr};
  TH1D* rfHistBuffer{nullptr};
  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    histBuffer = sample.getMcContainer().histogram.get();
    nominalHistBuffer = _nominalSamplesMcHistogram_[&sample].get();
    for( int iBin = 1 ; iBin <= histBuffer->GetNbinsX() ; iBin++ ){
      if( iBin % GlobalVariables::getNbThreads() != iThread_ ) continue;
      histBuffer->SetBinContent(iBin, nominalHistBuffer->GetBinContent(iBin));
    }
  }

  int iPar = -1;
  for( auto& parSet : _parameterSetsList_ ){
    for( auto& par : parSet.getParameterList() ){
      iPar++;
      double xSigmaPar = par.getDistanceFromNominal();
      if( xSigmaPar == 0 ) continue;

      for( auto& sample : _fitSampleSet_.getFitSampleList() ){
        histBuffer = sample.getMcContainer().histogram.get();
        nominalHistBuffer = _nominalSamplesMcHistogram_[&sample].get();
        rfHistBuffer = _responseFunctionsSamplesMcHistogram_[&sample][iPar].get();

        for( int iBin = 1 ; iBin <= histBuffer->GetNbinsX() ; iBin++ ){
          if( iBin % GlobalVariables::getNbThreads() != iThread_ ) continue;
          histBuffer->SetBinContent(
            iBin,
            histBuffer->GetBinContent(iBin) * ( 1 + xSigmaPar * rfHistBuffer->GetBinContent(iBin) )
          );
        }
      }
    }
  }

  for( auto& sample : _fitSampleSet_.getFitSampleList() ){
    histBuffer = sample.getMcContainer().histogram.get();
    nominalHistBuffer = _nominalSamplesMcHistogram_[&sample].get();
    for( int iBin = 1 ; iBin <= histBuffer->GetNbinsX() ; iBin++ ){
      if( iBin % GlobalVariables::getNbThreads() != iThread_ ) continue;
      histBuffer->SetBinError(iBin, TMath::Sqrt(histBuffer->GetBinContent(iBin)));
//      if( iThread_ == 0 ){
//        LogTrace << GET_VAR_NAME_VALUE(iBin)
//        << " / " << GET_VAR_NAME_VALUE(histBuffer->GetBinContent(iBin))
//        << " / " << GET_VAR_NAME_VALUE(nominalHistBuffer->GetBinContent(iBin))
//        << std::endl;
//      }
    }
  }

}
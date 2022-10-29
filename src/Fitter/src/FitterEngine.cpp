//
// Created by Nadrino on 11/06/2021.
//

#include "FitterEngine.h"
#include "JsonUtils.h"
#include "GlobalVariables.h"

#include "Logger.h"
#include "GenericToolbox.Root.h"
#include "GenericToolbox.h"
#include "GenericToolbox.TablePrinter.h"

#include <Math/Factory.h>
#include "TGraph.h"
#include "TLegend.h"
#include "TH1D.h"
#include "TBox.h"

#include <cmath>
#include <memory>


LoggerInit([]{
  Logger::setUserHeaderStr("[FitterEngine]");
});

FitterEngine::FitterEngine(TDirectory *saveDir_){
  this->setSaveDir(saveDir_); // propagate
}

void FitterEngine::readConfigImpl(){
  LogInfo << "Reading FitterEngine config..." << std::endl;
  GenericToolbox::setT2kPalette();

  _enablePca_ = JsonUtils::fetchValue(_config_, std::vector<std::string>{"enablePca", "fixGhostFitParameters"}, _enablePca_);
  _pcaDeltaChi2Threshold_ = JsonUtils::fetchValue(_config_, {{"ghostParameterDeltaChi2Threshold"}, {"pcaDeltaChi2Threshold"}}, _pcaDeltaChi2Threshold_);

  _enablePreFitScan_ = JsonUtils::fetchValue(_config_, "enablePreFitScan", _enablePreFitScan_);
  _enablePostFitScan_ = JsonUtils::fetchValue(_config_, "enablePostFitScan", _enablePostFitScan_);

  _generateSamplePlots_ = JsonUtils::fetchValue(_config_, "generateSamplePlots", _generateSamplePlots_);
  _generateOneSigmaPlots_ = JsonUtils::fetchValue(_config_, "generateOneSigmaPlots", _generateOneSigmaPlots_);
  _doAllParamVariations_ = JsonUtils::doKeyExist(_config_, "allParamVariations");
  _allParamVariationsSigmas_ = JsonUtils::fetchValue(_config_, "allParamVariations", _allParamVariationsSigmas_);

  _scaleParStepWithChi2Response_ = JsonUtils::fetchValue(_config_, "scaleParStepWithChi2Response", _scaleParStepWithChi2Response_);
  _parStepGain_ = JsonUtils::fetchValue(_config_, "parStepGain", _parStepGain_);

  _debugPrintLoadedEvents_ = JsonUtils::fetchValue(_config_, "debugPrintLoadedEvents", _debugPrintLoadedEvents_);
  _debugPrintLoadedEventsNbPerSample_ = JsonUtils::fetchValue(_config_, "debugPrintLoadedEventsNbPerSample", _debugPrintLoadedEventsNbPerSample_);

  _throwMcBeforeFit_ = JsonUtils::fetchValue(_config_, "throwMcBeforeFit", _throwMcBeforeFit_);
  _throwGain_ = JsonUtils::fetchValue(_config_, "throwMcBeforeFitGain", _throwGain_);

  _propagator_.readConfig( JsonUtils::fetchValue<nlohmann::json>(_config_, "propagatorConfig") );
  _minimizer_.readConfig( JsonUtils::fetchValue(_config_, "minimizerConfig", nlohmann::json()));


  // legacy
  if( JsonUtils::doKeyExist(_config_, "scanConfig") ){
    LogAlert << "\"scanConfig\" option should now be specified within propagatorConfig" << std::endl;
    LogAlert << "For LEGACY, parScanner config is provided. This will be deprecated in the future versions." << std::endl;
    _propagator_.getParScanner().readConfig( JsonUtils::fetchValue(_config_, "scanConfig", nlohmann::json()) );
  }

  _minimizer_.getConvergenceMonitor().setMaxRefreshRateInMs(JsonUtils::fetchValue(_config_, "monitorRefreshRateInMs", _minimizer_.getConvergenceMonitor().getMaxRefreshRateInMs()));
  LogInfo << "Convergence monitor will be refreshed every " << _minimizer_.getConvergenceMonitor().getMaxRefreshRateInMs() << "ms." << std::endl;
}
void FitterEngine::initializeImpl(){
  LogThrowIf(_config_.empty(), "Config is not set.");
  LogThrowIf(_saveDir_== nullptr);

  _propagator_.initialize();

  if( _debugPrintLoadedEvents_ ){
    LogDebug << GET_VAR_NAME_VALUE(_debugPrintLoadedEventsNbPerSample_) << std::endl;
    for( auto& sample : _propagator_.getFitSampleSet().getFitSampleList() ){
      LogDebug << "debugPrintLoadedEvents: " << sample.getName() << std::endl;
      LogDebug.setIndentStr("  ");

      int iEvt=0;
      for( auto& ev : sample.getMcContainer().eventList ){
        if(iEvt++ >= _debugPrintLoadedEventsNbPerSample_) { LogDebug << std::endl; break; }
        LogDebug << iEvt << " -> " << ev.getSummary() << std::endl;
      }
      LogDebug.setIndentStr("");
    }
  }

  // This moves the parameters
  if( _enablePca_ ) {
    LogWarning << "PCA is enabled. Polling parameters..." << std::endl;
    this->fixGhostFitParameters();
  }

  // This moves the parameters
  if( _scaleParStepWithChi2Response_ ){
    LogInfo << "Using parameter step scale: " << _parStepGain_ << std::endl;
    this->rescaleParametersStepSize();
  }

  // The minimizer needs all the parameters to be fully setup (i.e. PCA done and other properties)
  _minimizer_.initialize();

  if(GlobalVariables::getVerboseLevel() >= MORE_PRINTOUT) checkNumericalAccuracy();

  // Write data
  LogInfo << "Writing propagator objects..." << std::endl;
  GenericToolbox::writeInTFile(GenericToolbox::mkdirTFile(_saveDir_, "propagator"),
                               _propagator_.getGlobalCovarianceMatrix().get(), "globalCovarianceMatrix");
  for( auto& parSet : _propagator_.getParameterSetsList() ){
    if(not parSet.isEnabled()) continue;
    GenericToolbox::writeInTFile( GenericToolbox::mkdirTFile(_saveDir_, "propagator/"+parSet.getName()),
                                  parSet.getPriorCovarianceMatrix().get(), "covarianceMatrix");
    GenericToolbox::writeInTFile( GenericToolbox::mkdirTFile(_saveDir_, "propagator/"+parSet.getName()),
                                  parSet.getPriorCorrelationMatrix().get(), "correlationMatrix");
  }

  this->_propagator_.updateLlhCache();
  _propagator_.getTreeWriter().writeSamples(GenericToolbox::mkdirTFile(_saveDir_, "preFit/events"));
}

// Setters
void FitterEngine::setSaveDir(TDirectory *saveDir) {
  _saveDir_ = saveDir;
}
void FitterEngine::setIsDryRun(bool isDryRun_){
  _isDryRun_ = isDryRun_;
}
void FitterEngine::setEnablePca(bool enablePca_){
  _enablePca_ = enablePca_;
}
void FitterEngine::setEnablePreFitScan(bool enablePreFitScan) {
  _enablePreFitScan_ = enablePreFitScan;
}
void FitterEngine::setEnablePostFitScan(bool enablePostFitScan) {
  _enablePostFitScan_ = enablePostFitScan;
}
void FitterEngine::setGenerateSamplePlots(bool generateSamplePlots) {
  _generateSamplePlots_ = generateSamplePlots;
}
void FitterEngine::setGenerateOneSigmaPlots(bool generateOneSigmaPlots){
  _generateOneSigmaPlots_ = generateOneSigmaPlots;
}
void FitterEngine::setDoAllParamVariations(bool doAllParamVariations_){
  _doAllParamVariations_ = doAllParamVariations_;
}
void FitterEngine::setAllParamVariationsSigmas(const std::vector<double> &allParamVariationsSigmas) {
  _allParamVariationsSigmas_ = allParamVariationsSigmas;
}

// Getters
const Propagator& FitterEngine::getPropagator() const {
  return _propagator_;
}
Propagator& FitterEngine::getPropagator() {
  return _propagator_;
}

// Core
void FitterEngine::fit(){
  LogWarning << __METHOD_NAME__ << std::endl;
  LogThrowIf(not isInitialized());

  // Not moving parameters
  if( _generateSamplePlots_ ){
    LogInfo << "Generating pre-fit sample plots..." << std::endl;
    _propagator_.getPlotGenerator().generateSamplePlots(GenericToolbox::mkdirTFile(_saveDir_, "preFit/samples"));
    GenericToolbox::triggerTFileWrite(_saveDir_);
  }

  // Moving parameters
  if( _generateOneSigmaPlots_ ){
    LogInfo << "Generating pre-fit one-sigma variation plots..." << std::endl;
    _propagator_.getParScanner().generateOneSigmaPlots(GenericToolbox::mkdirTFile(_saveDir_, "preFit/oneSigma"));
  }
  if( _doAllParamVariations_ ){
    LogInfo << "Running all parameter variation on pre-fit samples..." << std::endl;
    _propagator_.getParScanner().varyEvenRates( _allParamVariationsSigmas_, GenericToolbox::mkdirTFile(_saveDir_, "preFit/varyEventRates") );
  }
  if( _enablePreFitScan_ ){
    LogInfo << "Scanning fit parameters before minimizing..." << std::endl;
    this->scanMinimizerParameters(GenericToolbox::mkdirTFile(_saveDir_, "preFit/scan"));
  }
  if( _throwMcBeforeFit_ ){
    LogInfo << "Throwing correlated parameters of MC away from their prior..." << std::endl;
    LogInfo << "Throw gain form MC push set to: " << _throwGain_ << std::endl;
    for( auto& parSet : _propagator_.getParameterSetsList() ){
      if(not parSet.isEnabled()) continue;
      if( not parSet.isEnabledThrowToyParameters() ){
        LogWarning << "\"" << parSet.getName() << "\" has marked disabled throwMcBeforeFit: skipping." << std::endl;
        continue;
      }
      if( JsonUtils::doKeyExist(parSet.getConfig(), "customFitParThrow") ){

        LogAlert << "Using custom mc parameter push for " << parSet.getName() << std::endl;

        for(auto& entry : JsonUtils::fetchValue(parSet.getConfig(), "customFitParThrow", std::vector<nlohmann::json>())){

          int parIndex = JsonUtils::fetchValue<int>(entry, "parIndex");

          auto& parList = parSet.getParameterList();
          double pushVal =
              parList[parIndex].getParameterValue()
              + parList[parIndex].getStdDevValue()
                * JsonUtils::fetchValue<double>(entry, "nbSigmaAway");

          LogWarning << "Pushing #" << parIndex << " to " << pushVal << std::endl;
          parList[parIndex].setParameterValue( pushVal );

          if( parSet.isUseEigenDecompInFit() ){
            parSet.propagateOriginalToEigen();
          }

        }
        continue;
      }
      else{
        LogAlert << "Throwing correlated parameters for " << parSet.getName() << std::endl;
        parSet.throwFitParameters(_throwGain_);
      }
    } // parSet
  }

  // Leaving now?
  if( _isDryRun_ ){
    LogAlert << "Dry run requested. Leaving before the minimization." << std::endl;
    return;
  }

  _minimizer_.minimize();

  if( _generateSamplePlots_ ){
    _propagator_.getPlotGenerator().generateSamplePlots(GenericToolbox::mkdirTFile(_saveDir_, "postFit/samples"));
  }
  if( _enablePostFitScan_ ){
    LogInfo << "Scanning fit parameters around the minimum point..." << std::endl;
    this->scanMinimizerParameters(GenericToolbox::mkdirTFile(_saveDir_, "postFit/scan"));
  }

  if( _minimizer_.isFitHasConverged() and _minimizer_.isEnablePostFitErrorEval() ){
    LogWarning << "Computing post-fit errors..." << std::endl;
    _minimizer_.calcErrors();
  }
  else{
    if( not _minimizer_.isFitHasConverged() ) LogAlert << "Skipping post-fit error calculation since the minimizer did not converge." << std::endl;
    else LogAlert << "Skipping post-fit error calculation since the option is disabled." << std::endl;
  }

  LogWarning << "Fit is done." << std::endl;
}

// protected
void FitterEngine::fixGhostFitParameters(){

  _propagator_.updateLlhCache();
  double baseChi2 = _propagator_.getLlhBuffer();
  double baseChi2Stat = _propagator_.getLlhStatBuffer();
  double baseChi2Syst = _propagator_.getLlhPenaltyBuffer();

  LogInfo << "Reference " << GUNDAM_CHI2 << "(stat) for PCA: " << baseChi2Stat << std::endl;

  // +1 sigma
  int iFitPar = -1;
  std::stringstream ssPrint;
  double deltaChi2Stat;

  for( auto& parSet : _propagator_.getParameterSetsList() ){

    if( not parSet.isEnabled() ){ continue; }

    if( not parSet.isEnablePca() ){
      LogWarning << "PCA disabled on " << parSet.getName() << ". Skipping..." << std::endl;
      continue;
    }
    else{
      LogInfo << "Performing PCA on " << parSet.getName() << "..." << std::endl;
    }

    bool fixNextEigenPars{false};
    auto& parList = parSet.getEffectiveParameterList();
    for( auto& par : parList ){
      ssPrint.str("");
      ssPrint << "(" << par.getParameterIndex()+1 << "/" << parList.size() << ") +1" << GUNDAM_SIGMA << " on " << parSet.getName() + "/" + par.getTitle();

      if( fixNextEigenPars ){
        par.setIsFixed(true);
#ifndef NOCOLOR
        std::string red(GenericToolbox::ColorCodes::redBackground);
        std::string rst(GenericToolbox::ColorCodes::resetColor);
#else
        std::string red;
        std::string rst;
#endif
//        LogInfo << red << ssPrint.str() << " -> FIXED AS NEXT EIGEN." << rst << std::endl;
        continue;
      }

      if( par.isEnabled() and not par.isFixed() ){
        double currentParValue = par.getParameterValue();
        par.setParameterValue( currentParValue + par.getStdDevValue() );

        ssPrint << " " << currentParValue << " -> " << par.getParameterValue();
        LogInfo << ssPrint.str() << "..." << std::endl;

        _propagator_.updateLlhCache();
        deltaChi2Stat = _propagator_.getLlhStatBuffer() - baseChi2Stat;

        ssPrint << ": " << GUNDAM_DELTA << GUNDAM_CHI2 << " (stat) = " << deltaChi2Stat;

        LogInfo.moveTerminalCursorBack(1);
        LogInfo << ssPrint.str() << std::endl;

        if( std::abs(deltaChi2Stat) < _pcaDeltaChi2Threshold_ ){
          par.setIsFixed(true); // ignored in the Chi2 computation of the parSet
          ssPrint << " < " << JsonUtils::fetchValue(_config_, {{"ghostParameterDeltaChi2Threshold"}, {"pcaDeltaChi2Threshold"}}, 1E-6) << " -> FIXED";
          LogInfo.moveTerminalCursorBack(1);
#ifndef NOCOLOR
          std::string red(GenericToolbox::ColorCodes::redBackground);
          std::string rst(GenericToolbox::ColorCodes::resetColor);
#else
          std::string red;
        std::string rst;
#endif
          LogInfo << red << ssPrint.str() << rst << std::endl;

          if( parSet.isUseEigenDecompInFit() and JsonUtils::fetchValue(_config_, "fixGhostEigenParmetersAfterFirstRejected", false) ){
            fixNextEigenPars = true;
          }
        }

        // Come back to the original values
        par.setParameterValue( currentParValue );
      }
    }

    // Recompute inverse matrix for the fitter
    // Note: Eigen decomposed parSet don't need a new inversion since the matrix is diagonal
    if( not parSet.isUseEigenDecompInFit() ){
      parSet.prepareFitParameters();
    }

  }

  // comeback to old values
  _propagator_.updateLlhCache();
}
void FitterEngine::rescaleParametersStepSize(){
  LogInfo << __METHOD_NAME__ << std::endl;

  _propagator_.updateLlhCache();
  double baseChi2Pull = _propagator_.getLlhPenaltyBuffer();
  double baseChi2 = _propagator_.getLlhBuffer();

  // +1 sigma
  int iFitPar = -1;
  for( auto& parSet : _propagator_.getParameterSetsList() ){

    for( auto& par : parSet.getEffectiveParameterList() ){
      iFitPar++;

      if( not par.isEnabled() ){
        continue;
      }

      double currentParValue = par.getParameterValue();
      par.setParameterValue( currentParValue + par.getStdDevValue() );

      _propagator_.updateLlhCache();

      double deltaChi2 = _propagator_.getLlhBuffer() - baseChi2;
      double deltaChi2Pulls = _propagator_.getLlhPenaltyBuffer() - baseChi2Pull;

      // Consider a parabolic approx:
      // only rescale with X2 stat?
//        double stepSize = TMath::Sqrt(deltaChi2Pulls)/TMath::Sqrt(deltaChi2);

      // full rescale
      double stepSize = 1./TMath::Sqrt(std::abs(deltaChi2));

      LogInfo << "Step size of " << parSet.getName() + "/" + par.getTitle()
              << " -> σ x " << _parStepGain_ << " x " << stepSize
              << " -> Δχ² = " << deltaChi2 << " = " << deltaChi2 - deltaChi2Pulls << "(stat) + " << deltaChi2Pulls << "(pulls)";

      stepSize *= par.getStdDevValue() * _parStepGain_;

      par.setStepSize( stepSize );
      par.setParameterValue( currentParValue + stepSize );
      _propagator_.updateLlhCache();
      LogInfo << " -> Δχ²(step) = " << _propagator_.getLlhBuffer() - baseChi2 << std::endl;
      par.setParameterValue( currentParValue );
    }

  }

  _propagator_.updateLlhCache();
}
void FitterEngine::scanMinimizerParameters(TDirectory* saveDir_){
  LogThrowIf(not isInitialized());
  LogInfo << "Performing scans of fit parameters..." << std::endl;
  for( int iPar = 0 ; iPar < _minimizer_.getMinimizer()->NDim() ; iPar++ ){
    if( _minimizer_.getMinimizer()->IsFixedVariable(iPar) ){
      LogWarning << _minimizer_.getMinimizer()->VariableName(iPar)
                 << " is fixed. Skipping..." << std::endl;
      continue;
    }
    _propagator_.getParScanner().scanFitParameter(*_minimizer_.getMinimizerFitParameterPtr()[iPar], saveDir_);
  } // iPar
}
void FitterEngine::checkNumericalAccuracy(){
  LogWarning << __METHOD_NAME__ << std::endl;
  int nTest{100}; int nThrows{10}; double gain{20};
  std::vector<std::vector<std::vector<double>>> throws(nThrows); // saved throws [throw][parSet][par]
  std::vector<double> responses(nThrows, std::nan("unset"));
  // stability/numerical accuracy test

  LogInfo << "Throwing..." << std::endl;
  for(auto& throwEntry : throws ){
    for( auto& parSet : _propagator_.getParameterSetsList() ){
      if(not parSet.isEnabled()) continue;
      if( not parSet.isEnabledThrowToyParameters() ){ continue;}
      parSet.throwFitParameters(gain);
      throwEntry.emplace_back(std::vector<double>(parSet.getParameterList().size(), 0));
      for( size_t iPar = 0 ; iPar < parSet.getParameterList().size() ; iPar++){
        throwEntry.back()[iPar] = parSet.getParameterList()[iPar].getParameterValue();
      }
      parSet.moveFitParametersToPrior();
    }
  }

  LogInfo << "Testing..." << std::endl;
  for( int iTest = 0 ; iTest < nTest ; iTest++ ){
    GenericToolbox::displayProgressBar(iTest, nTest, "Testing computational accuracy...");
    for( size_t iThrow = 0 ; iThrow < throws.size() ; iThrow++ ){
      int iParSet{-1};
      for( auto& parSet : _propagator_.getParameterSetsList() ){
        if(not parSet.isEnabled()) continue;
        if( not parSet.isEnabledThrowToyParameters() ){ continue;}
        iParSet++;
        for( size_t iPar = 0 ; iPar < parSet.getParameterList().size() ; iPar++){
          parSet.getParameterList()[iPar].setParameterValue( throws[iThrow][iParSet][iPar] );
        }
      }
      _propagator_.updateLlhCache();

      if( responses[iThrow] == responses[iThrow] ){ // not nan
        LogThrowIf( _propagator_.getLlhBuffer() != responses[iThrow], "Not accurate: " << _propagator_.getLlhBuffer() - responses[iThrow] << " / "
                                                                        << GET_VAR_NAME_VALUE(_propagator_.getLlhBuffer()) << " <=> " << GET_VAR_NAME_VALUE(responses[iThrow])
        )
      }
      responses[iThrow] = _propagator_.getLlhBuffer();
    }
    LogDebug << GenericToolbox::parseVectorAsString(responses) << std::endl;
  }
  LogInfo << "OK" << std::endl;
}

//
// Created by Clark McGrew on 26/01/2023.
//

#include "LikelihoodInterface.h"
#include "MCMCInterface.h"
#include "FitterEngine.h"
#include "GenericToolbox.Json.h"
#include "GlobalVariables.h"

#include "GenericToolbox.h"
#include "GenericToolbox.Root.h"
#include "Logger.h"

#include <locale>

LoggerInit([]{
  Logger::setUserHeaderStr("[MCMC]");
});

MCMCInterface::MCMCInterface(FitterEngine* owner_):
  MinimizerBase(owner_){}

void MCMCInterface::readConfigImpl(){
  MinimizerBase::readConfigImpl();
  LogInfo << "Reading minimizer config..." << std::endl;

  // The type of algorithm to be using.  It should be left at the default
  // value (metropolis is the only supported MCMC algorithm right now).
  _algorithmName_ = GenericToolbox::Json::fetchValue(_config_, "algorithm", _algorithmName_);

  // The step proposal algorithm.  This should usually be left at the default
  // value.
  _proposalName_ = GenericToolbox::Json::fetchValue(_config_, "proposal", _proposalName_);

  // The name of the MCMC result tree in the output file.  This doesn't need
  // to be changed.  Generally, leave it alone.
  _outTreeName_ = GenericToolbox::Json::fetchValue(_config_, "mcmcOutputTree", "MCMC");

  // Get the MCMC chain parameters to be used during burn-in.  The burn-in will
  // be skipped if the state has been restored from a file.  The burn-in can be
  // skipped in favor of discarding the initial parts of the MCMC chain
  // (usually a better option).  A run is broken into "mini-Chains" called a
  // "cycle" where the posterior covariance information is updated after each
  // mini-chain.  Each cycle will have "steps" steps.
  _burninCycles_ = GenericToolbox::Json::fetchValue(_config_,
                                         "burninCycles", _burninCycles_);

  // The number of steps to run in each burn in cycle
  _burninLength_ = GenericToolbox::Json::fetchValue(_config_,
                                         "burninSteps", _burninLength_);

  // If this is set to false, the burn-in steps will not be saved to disk.
  // This should usually be true since it lets you see the progress of the
  // burn-in.
  _saveBurnin_ = GenericToolbox::Json::fetchValue(_config_,
                                       "saveBurnin", _saveBurnin_);

  // Get the MCMC chain parameters.  A run is broken into "mini-Chains"
  // called a "cycle" where the posterior covariance information is updated
  // after each mini-chain.  Each cycle will have "steps" steps.
  _cycles_ = GenericToolbox::Json::fetchValue(_config_,
                                     "cycles", _cycles_);
  _steps_ = GenericToolbox::Json::fetchValue(_config_,
                                      "steps", _steps_);

  ///////////////////////////////////////////////////////////////
  // Get parameters for the adaptive proposal.

  // Set the window to calculate the current acceptance value over during
  // burn-in.  If this is set to short, the step size will fluctuate.  If this
  // is set to long, the step size won't be adjusted to match the target
  // acceptance.  Make this very large to lock the step size.
  _burninCovWindow_ = GenericToolbox::Json::fetchValue(
    _config_, "burninCovWindow", _burninCovWindow_);

  // The covariance deweighting during burn-in.  This should usually be left
  // at the default value.  This sets how much extra influence new points
  // should have on the covariance.
  _burninCovDeweighting_ = GenericToolbox::Json::fetchValue(
    _config_, "burninCovDeweighting", _burninCovDeweighting_);

  // The number of times that the burn-in state will be reset.  If this is
  // zero, then there are no resets (one means reset after the first cycle,
  // &c).  Resets are sometimes needed if the initial conditions are far from
  // the main probability in the posterior and the "best fit" parameters need
  // to be found.
  _burninResets_ = GenericToolbox::Json::fetchValue(_config_,
                                         "burninResets", _burninResets_);

  // Freeze the step size after this many burn-in chains.  This stops
  // adaptively adjusting the step size.
  _burninFreezeAfter_ = GenericToolbox::Json::fetchValue(
    _config_, "burninFreezeAfter", _burninFreezeAfter_);

  // Set the window to calculate the current acceptance value over during
  // burn-in.  If this is set to short, the step size will fluctuate.  If this
  // is set to long, the step size won't be adjusted to match the target
  // acceptance.  Make this very large to lock the step size.
  _burninWindow_ = GenericToolbox::Json::fetchValue(
    _config_, "burninWindow", _burninWindow_);

  // Set the name of a file containing an existing Markov chain to be
  // extended.  If this is set, then the burn-in will be skipped.
  //
  // The value is settable from the command line (setting from the command
  // line is the better option) using the override option
  //
  // "-O /fitterEngineConfig/mcmcConfig/adaptiveRestore=<filename>"
  //
  // If restore is going to be used, the adaptiveRestore value must exist in
  // the configuration file (with a NULL value)
  _adaptiveRestore_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveRestore", _adaptiveRestore_);
  _adaptiveRestore_
    = GenericToolbox::expandEnvironmentVariables(_adaptiveRestore_);

  // Set the name of a file containing a TH2D that describes the covariance of
  // the proposal distribution.
  //
  // The value is settable from the command line (setting from the command
  // line is the better option) using the override option
  //
  // "-O /fitterEngineConfig/mcmcConfig/adaptiveCovFile=<filename>"
  //
  // For this to be used the adaptiveCovFile value must exist in the
  // configuration file (with a value of "none" to be ignored, or a default
  // value if a default should be loaded)
  _adaptiveCovFile_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveCovFile", _adaptiveCovFile_);
  _adaptiveCovFile_
    = GenericToolbox::expandEnvironmentVariables(_adaptiveCovFile_);

  // Set the name of a ROOT TH2D that will be used as the covariance of the
  // step proposal.  If adaptiveCovFile is not set, or has a value of "none",
  // this will be ignored.
  _adaptiveCovName_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveCovName", _adaptiveCovName_);

  // Get the effective number of trials for a proposal covariance that is
  // being read from a file. This should typically be about 0.5*N^2 where N is
  // the dimension of the covariance.  That works out to the approximate
  // number of function calculations that were used to estimate the
  // covariance.  The default value of zero triggers the interface to make
  // it's own estimate.
  _adaptiveCovTrials_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveCovTrials", _adaptiveCovTrials_);

  // Set the window to calculate the current covariance value over.  If this
  // is set to short, the covariance will not sample the entire posterior.
  // Generally, the window should be long compared to the number of steps
  // required to get to an uncorrelated point.
  _adaptiveCovWindow_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveCovWindow", _adaptiveCovWindow_);

  // The covariance deweighting while the chain is running.  This should
  // usually be left at zero so the entire chain history is used after an
  // update and more recent points don't get a heavier weight (within the
  // covariance window).
  _adaptiveCovDeweighting_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveCovDeweighting", _adaptiveCovDeweighting_);

  // Set the initial rigidity for the changes in the step size.  If this is
  // negative, the step size is not updated as it runs. This stops adaptively
  // adjusting the step size.
  _adaptiveFreezeAfter_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveFreezeAfter", _adaptiveFreezeAfter_);

  // Set the window to calculate the current acceptance value over.  If this
  // is set to short, the step size will fluctuate a lot.  If this is set to
  // long, the step size won't be adjusted to match the target acceptance.
  // Make this very large effectively locks the step size.
  _adaptiveWindow_ = GenericToolbox::Json::fetchValue(
    _config_, "adaptiveWindow", _adaptiveWindow_);

  ///////////////////////////////////////////////////////////////
  // Get parameters for the simple proposal.

  // Set the step size for the simple proposal.
  _simpleSigma_ = GenericToolbox::Json::fetchValue(_config_,
                                        "simpleSigma", _simpleSigma_);
}

void MCMCInterface::initializeImpl(){
  MinimizerBase::initializeImpl();
  LogInfo << "Initializing the MCMC Integration..." << std::endl;

  // Configure the likelihood with the local settings.  These are used by the
  // likelihood to print informational messages, but do not affect how the
  // likelihood code runs.
  getLikelihood().setMinimizerInfo(_algorithmName_,_proposalName_);

}

/// An MCMC doesn't really converge in the sense meant here. This flags
/// success.
bool MCMCInterface::isFitHasConverged() const {return true;}

/// Copy the current parameter values to the tree.
void MCMCInterface::fillPoint() {
  int count = 0;
  for (const FitParameterSet& parSet: getPropagator().getParameterSetsList()) {
    for (const FitParameter& iPar : parSet.getParameterList()) {
      if (count >= _point_.size()) {
        LogWarning << "Point out of range " << _point_.size() << " " << count << std::endl;
        continue;
      }
      _point_[count++] = iPar.getParameterValue();
    }
  }
}

bool MCMCInterface::adaptiveRestoreState(AdaptiveStepMCMC& mcmc,
                                         const std::string& fileName,
                                         const std::string& treeName) {

  // No filename so, no restoration.
  if (fileName.empty()) return false;

  // Filename is "null"
  std::string tmp = fileName;
  std::use_facet<std::ctype<std::string::value_type>>(std::locale())
    .tolower(&tmp[0],&tmp[0]+tmp.size());
  if (tmp == "none") return false;

  // Open the file with the state.
  TFile* saveFile = gFile;
  std::unique_ptr<TFile> restoreFile
    (new TFile(fileName.c_str(), "old"));
  if (!restoreFile || !restoreFile->IsOpen()) {
    LogInfo << "File to restore was not found: "
            << fileName << std::endl;
    throw std::runtime_error("Old state file not open");
  }

  // Get the tree with the state.
  TTree* restoreTree = dynamic_cast<TTree*>(restoreFile->Get(treeName.c_str()));
  if (!restoreTree) {
    LogInfo << "Tree to restore state is not found in "
            << restoreFile->GetName()
            << treeName << std::endl;
    throw std::runtime_error("Old state tree not open");
  }

  LogInfo << "Restore the state of a previous MCMC and extend: "
          << restoreFile->GetName() << std::endl;

  // Load the old state from the tree.
  mcmc.Restore(restoreTree);
  LogInfo << "State Restored" << std::endl;

  // Set the deweighting to zero so set covariance is directly used.
  mcmc.GetProposeStep().SetCovarianceUpdateDeweighting(0.0);

  // Restore the original file status.
  gFile = saveFile;
  gFile->cd((std::string(owner().getSaveDir()->GetName())+"/fit").c_str());

  return true;
}

bool
MCMCInterface::adaptiveDefaultProposalCovariance(AdaptiveStepMCMC& mcmc,
                                                 Vector& prior) {

  /// Set the diagonal elements for the parameters.
  int count0 = 0;
  for (const FitParameter* par : getMinimizerFitParameterPtr() ) {
    ++count0;
    if (getLikelihood().getUseNormalizedFitSpace()) {
      // Changing the boundaries, change the value/step size?
      double step
        = FitParameterSet::toNormalizedParRange(
          par->getStepSize(), *par);
      mcmc.GetProposeStep().SetGaussian(count0-1,step);
    }
    else if (par->getPriorType() == PriorType::Flat) {
      // Gundam uses flat to mean "free", so this doesn't use a Uniform
      // step between bounds.
      double step = std::min(par->getStepSize(),par->getStdDevValue());
      if (step <= std::abs(1E-10*par->getPriorValue())) {
        step = std::max(par->getStepSize(),par->getStdDevValue());
      }
      step /= std::sqrt(getMinimizerFitParameterPtr().size());
      mcmc.GetProposeStep().SetGaussian(count0-1,step);
    }
    else {
      double step = par->getStdDevValue();
      mcmc.GetProposeStep().SetGaussian(count0-1,step);
    }
  }

  mcmc.GetProposeStep().ResetCorrelations();

  // Set up the correlations in the priors.
  int count1 = 0;
  for (const FitParameter* par1 : getMinimizerFitParameterPtr() ) {
    ++count1;
    const FitParameterSet* set1 = par1->getOwner();
    if (!set1) {
      LogInfo << "Parameter set reference is not defined for"
              << " " << par1->getName()
              << std::endl;
      continue;
    }
    if (set1->isUseEigenDecompInFit()) {
      continue;
    }

    int count2 = 0;
    for (const FitParameter* par2 : getMinimizerFitParameterPtr()) {
      ++count2;
      const FitParameterSet* set2 = par2->getOwner();
      if (!set2) {
        LogInfo << "Parameter set reference is not defined for"
                << " " << par1->getName()
                << std::endl;
        continue;
      }
      if (set2->isUseEigenDecompInFit()) {
        continue;
      }

      if (set1 != set2) continue;
      int in1 = par1->getParameterIndex();
      int in2 = par2->getParameterIndex();
      if (in2 <= in1) continue;
      const std::shared_ptr<TMatrixDSym>& corr
        = set1->getPriorCorrelationMatrix();
      if (!corr) continue;
      double correlation = (*corr)(in1,in2);
      // Don't impose very small correlations, let them be discovered.
      if (std::abs(correlation) < 0.01) continue;
      // Complain about large correlations.  When a correlation is this
      // large, then the user should (but probably won't) rethink the
      // parameter definitions!
      if (std::abs(correlation) > 0.98) {
        LogInfo << "VERY LARGE CORRELATION (" << correlation
                << ") BETWEEN"
                << " " << set1->getName() << "/" << par1->getName()
                << " & " << set2->getName() << "/" << par2->getName()
                << std::endl;
      }
      mcmc.GetProposeStep().SetCorrelation(count1-1,count2-1,
                                           (*corr)(in1,in2));
    }
  }

  return true;
}

bool
MCMCInterface::adaptiveLoadProposalCovariance(AdaptiveStepMCMC& mcmc,
                                              Vector& prior,
                                              const std::string& fileName,
                                              const std::string& histName) {
  // No filename so, no restoration.
  if (fileName.empty()) return false;

  // Filename is "null"
  std::string tmp = fileName;
  std::use_facet<std::ctype<std::string::value_type>>(std::locale())
    .tolower(&tmp[0],&tmp[0]+tmp.size());
  if (tmp == "none") return false;

  TFile* saveFile = gFile;
  std::unique_ptr<TFile> restoreFile
    (new TFile(fileName.c_str(), "old"));
  if (!restoreFile || !restoreFile->IsOpen()) {
    LogInfo << "File to restore was not found: "
            << fileName << std::endl;
    throw std::runtime_error("Old state file not open");
  }
  LogInfo << "Restore the covariance from external file: "
          << restoreFile->GetName() << std::endl;

  gFile = saveFile;
  gFile->cd((std::string(owner().getSaveDir()->GetName())+"/fit").c_str());

  LogInfo << "Set the value of the proposal covariance" << std::endl;
  TH2D* proposalCov = dynamic_cast<TH2D*>(
    restoreFile->Get(_adaptiveCovName_.c_str()));
  if (!proposalCov) {
    LogError << "Proposal TH2D not found "
             << _adaptiveCovName_ << std::endl;
    throw std::runtime_error("Covariance not in file");
  }

  // Check that the covariance that was read matchs the number of parameters.
  if (mcmc.GetProposeStep().GetDim() != proposalCov->GetNbinsX()
      or mcmc.GetProposeStep().GetDim() != proposalCov->GetNbinsY()) {
    LogError << "Loading proposal covariance with incorrect dimensions"
             << std::endl;
    LogError << "   Expected Dimensions: " << mcmc.GetProposeStep().GetDim()
             << std::endl;
    LogError << "   Proposal X Bins:     " << proposalCov->GetNbinsX()
             << std::endl;
    LogError << "   Proposal Y Bins:     " << proposalCov->GetNbinsY()
             << std::endl;
    LogThrow("Mismatched proposal covariance matrix");
  }

  // Dump all of the previous correlations.
  mcmc.GetProposeStep().ResetCorrelations();

  TAxis* covAxisLabels = dynamic_cast<TAxis*>(proposalCov->GetXaxis());
  int count1 = 0;
  for (const FitParameter* par1 : getMinimizerFitParameterPtr() ) {
    ++count1;
    std::string parName(par1->getFullTitle());
    std::string covName(covAxisLabels->GetBinLabel(count1));
    if (parName != covName) {
      LogError << "Mismatch of parameter and covariance names" << std::endl;
      LogError << "Parameter:  " << parName << std::endl;
      LogError << "Covariance: " << covName << std::endl;
      LogThrow("Mismatched covariance histogram");
    }
    double sig1 = std::sqrt(proposalCov->GetBinContent(count1,count1));
    int count2 = 0;
    for (const FitParameter* par2 : getMinimizerFitParameterPtr() ) {
      ++count2;
      double sig2 = std::sqrt(proposalCov->GetBinContent(count2,count2));
      if (count2 < count1) continue;
      else if (count2 == count1) {
        // Set the sigma for this variable
        double step = sig1;
#define COVARIANCE_NOT_IN_NORMALIZED_FIT_SPACE
#ifdef  COVARIANCE_NOT_IN_NORMALIZED_FIT_SPACE
        if (getLikelihood().getUseNormalizedFitSpace()) {
          step = FitParameterSet::toNormalizedParRange(sig1,*par1);
        }
#endif
        mcmc.GetProposeStep().SetGaussian(count1-1,step);
        continue;
      }
      double corr = proposalCov->GetBinContent(count1,count2)/sig1/sig2;
      mcmc.GetProposeStep().SetCorrelation(count1-1,count2-1,corr);
    }
  }

  // Set the effective number of trials for the covariance that was loaded.
  // The covariance is usually calculated by HESSE.  Empirically, HESSE calls
  // the function around constant plus half N^2 times.  Use that as the
  // initial number of trials.  This can also be set as a config file
  // parameter.
  double effectiveTrials = 100+0.5*count1*count1;
  LogInfo << "Setting effective number of trials "
          << effectiveTrials
          << "   User Request: " << _adaptiveCovTrials_
          << std::endl;
  if (_adaptiveCovTrials_ > 0) effectiveTrials = _adaptiveCovTrials_;
  _adaptiveCovTrials_ = effectiveTrials;
  mcmc.GetProposeStep().SetCovarianceTrials(_adaptiveCovTrials_);
  mcmc.GetProposeStep().SetEstimatedCenterTrials(_adaptiveCovTrials_);

  return true;
}

void MCMCInterface::setupAndRunAdaptiveStep(AdaptiveStepMCMC& mcmc) {

  mcmc.GetProposeStep().SetDim(getMinimizerFitParameterPtr().size());
  mcmc.GetLogLikelihood().functor = getLikelihood().evalFitValidFunctor();
  mcmc.GetProposeStep().SetCovarianceUpdateDeweighting(0.0);

  // Create a fitting parameter vector and initialize it.  No need to worry
  // about resizing it or it moving, so be lazy and just use push_back.
  Vector prior;
  for (const FitParameter* par : getMinimizerFitParameterPtr() ) {
    double val = par->getParameterValue();
    if (not getLikelihood().getUseNormalizedFitSpace()) {
      prior.push_back(val);
    }
    else {
      prior.push_back(FitParameterSet::toNormalizedParValue(val, *par));
    }
  }

  // Set the correlations in the default step proposal.
  if (not adaptiveLoadProposalCovariance(
        mcmc,prior,_adaptiveCovFile_,_adaptiveCovName_)) {;
    adaptiveDefaultProposalCovariance(mcmc,prior);
  }

  // Fill the initial point.
  fillPoint();

  // Initializing the mcmc sampler
  LogInfo << "Start with " << prior.size() << " parameters" << std::endl;
  mcmc.Start(prior, _saveBurnin_);
  mcmc.GetProposeStep().SetAcceptanceWindow(_adaptiveWindow_);
  mcmc.SetStepRMSWindow(_adaptiveWindow_);

  // Restore the chain if exist, otherwise, burn-in
  std::string restorationTree = "FitterEngine/fit/" + _outTreeName_;
  bool restored = adaptiveRestoreState(mcmc,_adaptiveRestore_, restorationTree);
  if (not restored and _burninCycles_ > 0 and _burninLength_ > 0) {
    // Burn-In cycles
    mcmc.GetProposeStep().SetCovarianceWindow(_burninCovWindow_);
    mcmc.GetProposeStep().SetAcceptanceWindow(_burninWindow_);
    mcmc.SetStepRMSWindow(_burninWindow_);
    mcmc.GetProposeStep()
      .SetCovarianceUpdateDeweighting(_burninCovDeweighting_);
    for (int chain = 0; chain < _burninCycles_; ++chain){
      LogInfo << "Start burn-In chain " << chain << std::endl;
      mcmc.GetProposeStep().UpdateProposal();
      // Override default number of steps until the next automatic
      // UpdateProposal call.  This disables automatic updates during adaptive
      // burn-in.
      mcmc.GetProposeStep().SetNextUpdate(2*_burninLength_);
      mcmc.GetProposeStep()
        .SetCovarianceUpdateDeweighting(_burninCovDeweighting_);
      if (chain < _burninFreezeAfter_) {
        LogInfo << "Burn-in step size variance will be updated" << std::endl;
        mcmc.GetProposeStep().SetAcceptanceRigidity(2.0);
      }
      else {
        LogInfo << "Burn-in step size variance is frozen" << std::endl;
        mcmc.GetProposeStep().SetAcceptanceRigidity(-1);
      }
      // Burn-In chain in each cycle
      for (int i = 0; i < _burninLength_; ++i) {
        // Run step
        if (mcmc.Step(false)) fillPoint();
        // Now save the step.  Check to see if this is the last step of the
        // run, and if it is, then save the full state.
        if (_saveBurnin_) mcmc.SaveStep(_burninLength_ <= (i+1));
        if(_burninLength_ > 100 && !(i%(_burninLength_/100))){
          LogInfo << "Burn-in: " << chain
                  << " step: " << i << "/" << _burninLength_ << " "
                  << i*100./_burninLength_ << "%"
                  << " Trials: "
                  << mcmc.GetProposeStep().GetSuccesses()
                  << "/" << mcmc.GetProposeStep().GetTrials()
                  << " (acc " << mcmc.GetProposeStep().GetAcceptance()
                  << ", sig " << mcmc.GetProposeStep().GetSigma()
                  << ", rms " << mcmc.GetStepRMS()
                  << ")"
                  << std::endl;
        }
      }
      // Reset the covariance to the initial state.  This forgets the path of
      // the previous cycle.  This only happens a few times to let it forget
      // about the very first burn-in cycles
      if (chain < _burninResets_) {
        Vector saveCenter{mcmc.GetProposeStep().GetEstimatedCenter()};
        mcmc.GetProposeStep().ResetProposal();
        // After the reset, set how many trials the prior covariance counts
        // for.  This needs to be done by hand since the extra weight lives
        // here, and not in TSimpleMCMC.h (TSimpleMCMC doesn't, and shouldn't,
        // know about this burn-in process).
        if (_adaptiveCovTrials_ > 0) {
          mcmc.GetProposeStep().SetCovarianceTrials(_adaptiveCovTrials_);
          // We have a good prior (usually based on a previous MINUIT fit), so
          // restore the current estimate of the centeral value.
          mcmc.GetProposeStep().SetEstimatedCenter(saveCenter);
          mcmc.GetProposeStep().SetEstimatedCenterTrials(_adaptiveCovTrials_);
        }
      }
    }
    LogInfo << "Finished burn-in chains" << std::endl;
  }

  // Run cycles
  mcmc.GetProposeStep().SetCovarianceWindow(_adaptiveCovWindow_);
  mcmc.GetProposeStep().SetAcceptanceWindow(_adaptiveWindow_);
  mcmc.SetStepRMSWindow(_adaptiveWindow_);
  mcmc.GetProposeStep()
    .SetCovarianceUpdateDeweighting(_adaptiveCovDeweighting_);
  for (int chain = 0; chain < _cycles_; ++chain){
    LogInfo << "Start run chain " << chain << std::endl;
    // Update the covariance with the steps from the last cycle.  This
    // starts a new "reversible-chain".
    mcmc.GetProposeStep().UpdateProposal();
    // Override default number of steps until the next automatic
    // UpdateProposal call.  This disables automatic updates during normal
    // chains.
    mcmc.GetProposeStep().SetNextUpdate(2*_steps_);
    // Set the adaptive covariance deweighting after the first update so that
    // the previous deweighting is used for the first update.  This is a very
    // cheap call so set it on every iteration.
    mcmc.GetProposeStep()
      .SetCovarianceUpdateDeweighting(_adaptiveCovDeweighting_);
    if (chain < _adaptiveFreezeAfter_) {
      LogWarning << "Step size variance will be updated" << std::endl;
      mcmc.GetProposeStep().SetAcceptanceRigidity(2.0);
    }
    else {
      LogInfo << "Step size variance is frozen" << std::endl;
      mcmc.GetProposeStep().SetAcceptanceRigidity(-1);
    }
    // Run chain in each cycle
    for (int i = 0; i < _steps_; ++i) {
      // Run step, but do not save the step.  The step isn't saved so the
      // accepted step can be copied into the points (which will have
      // any decomposition removed).
      if (mcmc.Step(false)) fillPoint();
      // Now save the step.
      mcmc.SaveStep(false);
      if(_steps_ > 100 && !(i%(_steps_/100))){
        LogInfo << "Chain: " << chain
                << " step: " << i << "/" << _steps_ << " "
                << i*100./_steps_ << "%"
                << " Trials: "
                << mcmc.GetProposeStep().GetSuccesses()
                << "/" << mcmc.GetProposeStep().GetTrials()
                << " (acc " << mcmc.GetProposeStep().GetAcceptance()
                << ", sig " << mcmc.GetProposeStep().GetSigma()
                << ", rms " << mcmc.GetStepRMS()
                << ")"
                << std::endl;
      }
    }
    // Save the final state.  This step should be skipped when analyzing the
    // chain, the steps can be identified since the covariance is not empty.
    LogInfo << "Chain: " << chain << " complete"
            << " Run Length: " << _steps_
            << " -- Saving state"
            << std::endl;
    mcmc.SaveStep(true);
  }
  LogInfo << "Finished Running chains" << std::endl;

}

void MCMCInterface::setupAndRunSimpleStep(SimpleStepMCMC& mcmc) {

  mcmc.GetProposeStep().SetDim(getMinimizerFitParameterPtr().size());
  mcmc.GetLogLikelihood().functor = getLikelihood().evalFitFunctor();
  mcmc.GetProposeStep().fSigma = _simpleSigma_;

  Vector prior;
  for (const FitParameter* par : getMinimizerFitParameterPtr() ) {
    prior.push_back(par->getPriorValue());
  }

  // Fill the initial point.
  fillPoint();

  // Initializing the mcmc sampler
  mcmc.Start(prior, _saveBurnin_);

  // Burn-In cycles
  for (int chain = 0; chain < _burninCycles_; ++chain){
    LogInfo << "Start Burn-In Cycle " << chain << std::endl;
    // Burn-In chain in each cycle
    for (int i = 0; i < _burninLength_; ++i) {
      // Run step
      if (mcmc.Step(false)) fillPoint();
      // Save the step if burn-in steps are being saved..  Check to see if this
      // is the last step of the run, and if it is, then save the full state.
      if (_saveBurnin_) mcmc.SaveStep(_burninLength_ <= (i+1));
      if(_burninLength_ > 100 && !(i%(_burninLength_/100))){
        LogInfo << "Burn-in: " << chain
                << " step: " << i << "/" << _burninLength_ << " "
                << i*100./_burninLength_ << "%"
                << std::endl;
      }
    }
  }

  // Run cycles
  for (int chain = 0; chain < _cycles_; ++chain){
    LogInfo << "Start Main Cycle " << chain << std::endl;
    // Update the covariance with the steps from the last cycle.  This
    // starts a new "reversible-chain".
    mcmc.GetProposeStep().UpdateProposal();
    // Run chain in each cycle
    for (int i = 0; i < _steps_; ++i) {
      // Run step, but do not save the step.  The step isn't saved so the
      // accepted step can be copied into the points (which will have
      // any decomposition removed).
      if (mcmc.Step(false)) fillPoint();
      // Now save the step.  If this is the last step in the cycle, do a full
      // save.
      if (i < (_steps_-1)) mcmc.SaveStep(false);
      else mcmc.SaveStep(true);
      if(_steps_ > 100 && !(i%(_steps_/100))){
        LogInfo << "Chain: " << chain
                << " step: " << i << "/" << _steps_ << " "
                << i*100./_steps_ << "%"
                << std::endl;
      }
    }

    LogInfo << "Chain: " << chain << " complete"
            << " Run Length: " << _steps_
            << " -- Saving state"
            << std::endl;
  }
  LogInfo << "Finished Running chains" << std::endl;

}

void MCMCInterface::minimize(){
  LogThrowIf(not isInitialized(), "not initialized");

  printMinimizerFitParameters();

  // Update to the current parameter settings and the likelihood cache.
  getPropagator().updateLlhCache();

  GenericToolbox::mkdirTFile(owner().getSaveDir(), "fit")->cd();
  // Create output tree in the existing file
  LogInfo << "Adding MCMC information to file " << gFile->GetName()
          << std::endl;

  // Store parameter names as a tree in the currentdirectory
  TTree *parameterSetsTree = new TTree("parameterSets",
                                       "Tree of Parameter Set Information");
  std::vector<std::string> parameterSetNames;
  std::vector<int> parameterSetOffsets;
  std::vector<int> parameterSetCounts;
  std::vector<int> parameterIndex;
  std::vector<bool> parameterFixed;
  std::vector<bool> parameterEnabled;
  std::vector<std::string> parameterName;
  std::vector<double> parameterPrior;
  std::vector<double> parameterSigma;
  std::vector<double> parameterMin;
  std::vector<double> parameterMax;
  parameterSetsTree->Branch("parameterSetNames", &parameterSetNames);
  parameterSetsTree->Branch("parameterSetOffsets", &parameterSetOffsets);
  parameterSetsTree->Branch("parameterSetCounts", &parameterSetCounts);
  parameterSetsTree->Branch("parameterIndex", &parameterIndex);
  parameterSetsTree->Branch("parameterFixed", &parameterFixed);
  parameterSetsTree->Branch("parameterEnabled", &parameterEnabled);
  parameterSetsTree->Branch("parameterName", &parameterName);
  parameterSetsTree->Branch("parameterPrior", &parameterPrior);
  parameterSetsTree->Branch("parameterSigma", &parameterSigma);
  parameterSetsTree->Branch("parameterMin", &parameterMin);
  parameterSetsTree->Branch("parameterMax", &parameterMax);

  // Pull all of the parameters out of the parameter sets and save the
  // names, index, priors, and sigmas to the output.  The order in these
  // vectors define the parameters that are saved in the Points vector in
  // the output file.  Note that these parameters do not define the meaning
  // of the parameters in the call to the likelihood function.  Those
  // parameters are defined by the _minimizerFitParameterPtr_ vector.

  for (const FitParameterSet& parSet: getPropagator().getParameterSetsList()) {
    // Save name of parameter set
    parameterSetNames.push_back(parSet.getName());
    parameterSetOffsets.push_back(parameterIndex.size());

    int countParameters = 0;
    for (const FitParameter& iPar : parSet.getParameterList()) {
      ++countParameters;
      parameterIndex.push_back(iPar.getParameterIndex());
      parameterFixed.push_back(iPar.isFixed());
      parameterEnabled.push_back(iPar.isEnabled());
      parameterName.push_back(iPar.getTitle());
      parameterPrior.push_back(iPar.getPriorValue());
      parameterSigma.push_back(iPar.getStdDevValue());
      parameterMin.push_back(iPar.getMinValue());
      parameterMax.push_back(iPar.getMaxValue());
    }
    parameterSetCounts.push_back(countParameters);
  }
  parameterSetsTree->Fill();
  parameterSetsTree->Write();
  // End Storing parameter name informations

  _point_.resize(parameterName.size());
  LogInfo << "Parameters in likelihood: " << _point_.size() << std::endl;

  // Create the output tree for the accepted points.
  TTree *outputTree = new TTree(_outTreeName_.c_str(),
                                "Tree of accepted points");
  outputTree->Branch("Points",&_point_);

  // Run a chain.
  int nbFitCallOffset = getLikelihood().getNbFitCalls();
  LogInfo << "Fit call offset: " << nbFitCallOffset << std::endl;

  // Create the TSimpleMCMC object and call the specific runner.
  if (_proposalName_ == "adaptive") {
    TSimpleMCMC<PrivateProxyLikelihood,TProposeAdaptiveStep> mcmc(outputTree);
    setupAndRunAdaptiveStep(mcmc);
  }
  else if (_proposalName_ == "simple") {
    TSimpleMCMC<PrivateProxyLikelihood,TProposeSimpleStep> mcmc(outputTree);
    setupAndRunSimpleStep(mcmc);
  }

  int nbMCMCCalls = getLikelihood().getNbFitCalls() - nbFitCallOffset;

 // lasting printout
  LogInfo << getConvergenceMonitor().generateMonitorString();
  LogInfo << "MCMC ended after " << nbMCMCCalls << " calls." << std::endl;

  // Save the sampled points to the outputfile
  if (outputTree) outputTree->Write();

}

void MCMCInterface::calcErrors() {
    LogWarning << "Errors not calculated with MCMC" << std::endl;
}

void MCMCInterface::scanParameters(TDirectory* saveDir_) {
  LogThrowIf(not isInitialized());
  LogInfo << "Performing scans of fit parameters..." << std::endl;
  for(FitParameter* par : getMinimizerFitParameterPtr()) {
    getPropagator().getParScanner().scanFitParameter(*par, saveDir_);
  }
}

//  A Lesser GNU Public License

//  Copyright (C) 2023 GUNDAM DEVELOPERS

//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.

//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.

//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the
//
//  Free Software Foundation, Inc.
//  51 Franklin Street, Fifth Floor,
//  Boston, MA  02110-1301  USA

// Local Variables:
// mode:c++
// c-basic-offset:2
// compile-command:"$(git rev-parse --show-toplevel)/cmake/gundam-build.sh"
// End:

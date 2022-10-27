//
// Created by Nadrino on 11/06/2021.
//

#ifndef GUNDAM_FITTERENGINE_H
#define GUNDAM_FITTERENGINE_H


#include "Propagator.h"
#include "MinimizerInterface.h"
#include "JsonBaseClass.h"

#include "GenericToolbox.VariablesMonitor.h"
#include "GenericToolbox.CycleTimer.h"

#include "TDirectory.h"
#include "Math/Functor.h"
#include "Math/Minimizer.h"
#include "nlohmann/json.hpp"

#include "string"
#include "vector"
#include "memory"


class FitterEngine : public JsonBaseClass {

public:
  explicit FitterEngine(TDirectory *saveDir_);

  // Setters
  void setSaveDir(TDirectory *saveDir);
  void setEnablePreFitScan(bool enablePreFitScan);
  void setEnablePostFitScan(bool enablePostFitScan);
  void setEnablePca(bool enablePca_);

  // Getters
  const Propagator& getPropagator() const;
  Propagator& getPropagator();
  MinimizerInterface& getMinimizer(){ return _minimizer_; }
  TDirectory* getSaveDir(){ return _saveDir_; }

  // Core
  void fit();

protected:
  void readConfigImpl() override;
  void initializeImpl() override;

  void fixGhostFitParameters();
  void rescaleParametersStepSize();
  void scanMinimizerParameters(TDirectory* saveDir_);
  void checkNumericalAccuracy();


private:
  // Parameters
  bool _enablePca_{false};
  bool _throwMcBeforeFit_{false};
  bool _enablePreFitScan_{false};
  bool _enablePostFitScan_{false};
  bool _scaleParStepWithChi2Response_{false};
  bool _debugPrintLoadedEvents_{false};
  int _debugPrintLoadedEventsNbPerSample_{10};
  double _throwGain_{1.};
  double _parStepGain_{0.1};
  double _pcaDeltaChi2Threshold_{1E-6};

  // Internals
  TDirectory* _saveDir_{nullptr};
  Propagator _propagator_{};
  MinimizerInterface _minimizer_{this};

};


#endif //GUNDAM_FITTERENGINE_H

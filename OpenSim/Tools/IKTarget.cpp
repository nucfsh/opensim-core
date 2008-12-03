// IKTarget.cpp
// Authors: Ayman Habib, Peter Loan, Eran Guendelman
/* Copyright (c)  2005, Stanford University, Ayman Habib, Peter Loan, and Eran Guendelman.
* Use of the OpenSim software in source form is permitted provided that the following
* conditions are met:
* 	1. The software is used only for non-commercial research and education. It may not
*     be used in relation to any commercial activity.
* 	2. The software is not distributed or redistributed.  Software distribution is allowed 
*     only through https://simtk.org/home/opensim.
* 	3. Use of the OpenSim software or derivatives must be acknowledged in all publications,
*      presentations, or documents describing work in which OpenSim or derivatives are used.
* 	4. Credits to developers may not be removed from executables
*     created from modifications of the source.
* 	5. Modifications of source code must retain the above copyright notice, this list of
*     conditions and the following disclaimer. 
* 
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
*  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
*  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
*  SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
*  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; 
*  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
*  OR BUSINESS INTERRUPTION) OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
*  WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <OpenSim/Common/rdMath.h>
#include <OpenSim/Common/Storage.h>
#include <OpenSim/Common/SimmMacros.h>
#include <OpenSim/Simulation/Model/AbstractDynamicsEngine.h>
#include <OpenSim/Simulation/Model/AbstractCoordinate.h>
#include <OpenSim/Simulation/Model/MarkerSet.h>
#include <OpenSim/Common/InterruptedException.h>
#include "IKTaskSet.h"
#include "IKCoordinateTask.h"
#include "IKMarkerTask.h"
#include "IKTarget.h"
#include <SimTKlapack.h>

using namespace std;
using namespace OpenSim;
using SimTK::Vec3;
const double IKTarget::_perturbation=1e-3; 

//=============================================================================
// CONSTRUCTOR(S) AND DESTRUCTOR
//=============================================================================
//_____________________________________________________________________________
/**
 * Constructor
 */
static bool debug = false; // used for debugging
/*
 * used for debugging to tell if the code is in computePerformanceGradient context
*/
static bool calcDerivs = true; 

IKTarget::IKTarget(Model &aModel, IKTaskSet &aIKTaskSet, Storage& aExperimentalDataStorage):
_model(aModel),
_ikTaskSet(aIKTaskSet),
_experimentalDataStorage(aExperimentalDataStorage),
_markers(NULL),
_interrupted(false)
{
	buildMarkerMap(aExperimentalDataStorage.getColumnLabels());
	buildCoordinateMap(aExperimentalDataStorage.getColumnLabels());

	/** Number of controls -- also allocates _dx. */
	setNumParameters(_unprescribedQs.getSize());

	for (int i = 0; i < getNumParameters(); i++)
		_dx[i] = _perturbation;

	_printPerformanceValues = false;
}

//_____________________________________________________________________________
/**
 * Destructor.
 */
IKTarget::
~IKTarget(void)
{
	for(int i=0; i<_markers.getSize(); i++) delete _markers[i];
	for(int i=0; i<_prescribedQs.getSize(); i++) delete _prescribedQs[i];
	for(int i=0; i<_unprescribedQs.getSize(); i++) delete _unprescribedQs[i];
	// don't delete contents of _unprescribedWeightedQs since those are a subset of _unprescribedQs
}

//=============================================================================
// Optimization framework methods
//=============================================================================
//_____________________________________________________________________________
/**
 * Compute objective function (sum of squared errors in marker positions) using
 * current values (x) for controls. Value of error is returned in p
 */
int IKTarget::objectiveFunc(const SimTK::Vector &x, const bool new_parameters, SimTK::Real &f) const
{
	if(_interrupted) throw InterruptedException();

	// Assemble model in new configuration
	// x contains values only for unprescribed coordinates
	for (int i = 0; i < getNumParameters(); i++)
		_unprescribedQs[i]->coord->setValue(x[i], i==(getNumParameters()-1));
	if(debug) {
		for (int i = 0; i < getNumParameters(); i++)
			cout << _unprescribedQs[i]->coord->getName() << " = " << _unprescribedQs[i]->coord->getValue() << endl;
	}

	// Tally the square of the errors from markers
	double totalWeightedSquaredErrors = 0.0;
	double totalMarkerSquaredErrors = 0.0;
	double totalCoordinateSquaredErrors = 0.0;
	double maxMarkerError = 0.0, maxCoordinateError = 0.0; // these are the max unweighted errors
	int worstMarker = -1, worstCoordinate = -1;

	AbstractDynamicsEngine& de = _model.getDynamicsEngine();

	// We keep track of worst marker for debugging/tuning purposes
	for (int i = 0; i < _markers.getSize(); i++)
	{
		if(!_markers[i]->validExperimentalPosition) continue;

		// Get marker offset in local frame
		SimTK::Vec3 localPos;
		_markers[i]->marker->getOffset(localPos);

		// transform local marker to world frame
		SimTK::Vec3 globalPos;
		de.transformPosition(*_markers[i]->body, localPos, globalPos);

		double markerError = 0.0;
		for (int j = 0; j < 3; j++)
		{
			_markers[i]->computedPosition[j] = globalPos[j];
			double err = _markers[i]->experimentalPosition[j] - _markers[i]->computedPosition[j];
			markerError += (err * err);
		}

		totalMarkerSquaredErrors += markerError;
		if (markerError > maxMarkerError)
		{
			maxMarkerError = markerError;
			worstMarker = i;
		}

		totalWeightedSquaredErrors += _markers[i]->weight * markerError;

		if (debug)
			cout << _markers[i]->marker->getName() << " w = " << _markers[i]->weight 
				  << " exp = " << _markers[i]->experimentalPosition[0] << " " << _markers[i]->experimentalPosition[1] << " " << _markers[i]->experimentalPosition[2]
				  << " comp + " << globalPos[0] << " " << globalPos[1] << " " << globalPos[2] << endl;
	}

	for (int i = 0; i < _unprescribedWeightedQs.getSize(); i++)
	{
		double experimentalValue = _unprescribedWeightedQs[i]->experimentalValue;
		double computedValue = _unprescribedWeightedQs[i]->coord->getValue();
		double err = experimentalValue - computedValue;
		double coordinateError = err * err;

		totalCoordinateSquaredErrors += coordinateError;
		if (coordinateError > maxCoordinateError)
		{
			maxCoordinateError = coordinateError;
			worstCoordinate = i;
		}

		totalWeightedSquaredErrors += _unprescribedWeightedQs[i]->weight * coordinateError;

		if (debug)
			cout << _unprescribedWeightedQs[i]->coord->getName() << " w = " << _unprescribedWeightedQs[i]->weight << " exp = " << experimentalValue << " comp + " << computedValue << endl;
	}

	if (_printPerformanceValues || (!calcDerivs && debug))
	{
		cout << "total weighted squared error = " << totalWeightedSquaredErrors;
		if(totalMarkerSquaredErrors>0) {
			cout << ", marker error: RMS=" << sqrt(totalMarkerSquaredErrors/_markers.getSize());
			if (worstMarker >= 0) cout << ", max=" << sqrt(maxMarkerError) << " (" << _markers[worstMarker]->marker->getName() << ")";
		}
		if(totalCoordinateSquaredErrors>0) {
			cout << ", coord error: RMS=" << sqrt(totalCoordinateSquaredErrors/_unprescribedWeightedQs.getSize());
			if (worstCoordinate >= 0) cout << ", max=" << sqrt(maxCoordinateError) << " (" << _unprescribedWeightedQs[worstCoordinate]->coord->getName() << ")";
		}
		cout << endl;
		setErrorReportingQuantities(
			maxMarkerError, 
			(worstMarker<0)?"":_markers[worstMarker]->marker->getName(),
			maxCoordinateError, 
			(worstCoordinate<0)?"":_unprescribedWeightedQs[worstCoordinate]->coord->getName());
	}

	f = totalWeightedSquaredErrors;

	return 0;
}

//_____________________________________________________________________________
/**
 * Compute derivative of objective function using finite differences
 */
int IKTarget::gradientFunc(const SimTK::Vector &x, const bool new_parameters, SimTK::Vector &dpdx) const
{
	calcDerivs=true;
	int status = rdOptimizationTarget::CentralDifferences(this,&_dx[0],x,dpdx);
	calcDerivs=false;

	return (status);
}
//_____________________________________________________________________________
/**
 * Compute derivative of objective function using finite differences
 */
int IKTarget::iterativeOptimization(SimTK::Vector &results)
{
	// Iterative optimization method
	//
    //    de = J*dQ , which can be rearranged as: dQ = J^-1 * de , where J^-1 = (J'*J)^-1*J'
	//
	SimTK::Matrix J((3*_markers.getSize()+_unprescribedWeightedQs.getSize()), getNumParameters());
	//SimTK::Matrix Jinv(getNumParameters(), (3*_markers.getSize()+_unprescribedWeightedQs.getSize()));

	// Tally the square of the errors from markers
	double totalWeightedSquaredErrors = 0.0;
	double totalMarkerSquaredErrors = 0.0;
	double totalCoordinateSquaredErrors = 0.0;
	double maxMarkerError = 0.0, maxCoordinateError = 0.0; // these are the max unweighted errors
	int worstMarker = -1, worstCoordinate = -1;

	// Compute the change in Q's to reduce error in Marker Positions
	SimTK::Matrix dQ(getNumParameters(), 1); 
	SimTK::Matrix dError((3*_markers.getSize()+_unprescribedWeightedQs.getSize()), 1);
	SimTK::Real errorTol = 1e-4;
	int maxIter = 1000;
	int iter = 0;

	int info;
	SimTK::Matrix performanceMatrixCopy;
	SimTK::Vector performanceVectorCopy;
	int m = (3*_markers.getSize()+_unprescribedWeightedQs.getSize());
	int n = getNumParameters();
	int nrhs = 1;
	int lwork = max(min(m, n)+3*n+1, 2*min(m, n)+nrhs);
	SimTK::Vector lapackWork(lwork);
	double rcond = 1.0e-9;
	int rank = getNumParameters();
	int *jpvt = new int[n];

	SimTK::Real previousDErrorNorm = 0.0;
	SimTK::Real currentDErrorNorm = 0.0;
	SimTK::Real tempDErrorNorm = 0.0;
	SimTK::Real deltaDErrorNorm = 100.0;

	// Set matrices and vectors to zero
	J = 0;
	dQ = 0;
	dError = 0;
	lapackWork = 0;

	// Compute first dError
	for (int i = 0; i < getNumParameters(); i++)
	{
		_unprescribedQs[i]->coord->setValue(results[i], i==(getNumParameters()-1));
	}

	AbstractDynamicsEngine& de = _model.getDynamicsEngine();

	for (int i = 0; i < _markers.getSize(); i++)
	{
		if(!_markers[i]->validExperimentalPosition) continue;

		// Get marker offset in local frame
		Vec3 localPos;
		_markers[i]->marker->getOffset(localPos);

		// transform local marker to world frame
		Vec3 globalPos;
		de.transformPosition(*_markers[i]->body, localPos, globalPos);

		for (int r=0; r<3; r++)
		{
			_markers[i]->computedPosition[r] = globalPos[r];
			dError(i*3+r, 0) = sqrt(_markers[i]->weight) * (_markers[i]->experimentalPosition[r] - _markers[i]->computedPosition[r]); 
		}
	}

	// Q ERRORS
	int row = 3*_markers.getSize();
	for (int i = 0; i < getNumParameters(); i++) {
		if(_unprescribedQs[i]->weight) {
			dError(row, 0) = sqrt(_unprescribedQs[i]->weight) * (_unprescribedQs[i]->experimentalValue - _unprescribedQs[i]->coord->getValue());
			row++;
		}
	}

	currentDErrorNorm = dError.norm();

	// Change configuration by dQ until dError is small
	while(deltaDErrorNorm>errorTol && iter<maxIter)
	{
		previousDErrorNorm = currentDErrorNorm;

		//// Pseudo-inverse method
		//J = 0;
		//Jinv = 0;
		//createJacobian(results, J);
		//createPseudoInverseJacobian(J, Jinv);
		//dQ = Jinv*dError;

		// Linear least squares method
        createJacobian(results, J);
		performanceMatrixCopy = J;
		performanceVectorCopy = dError.col(0);
		for(int i=0; i<n; i++) { jpvt[i] = 0; }
		dgelsy_(m, n, nrhs, &performanceMatrixCopy(0,0), m, &performanceVectorCopy[0], m, &jpvt[0], rcond, rank, &lapackWork[0], lapackWork.size(), info);
		dQ.updCol(0) = performanceVectorCopy(0, getNumParameters());

		if(rank < n){
			cout << "\nIKTarget.iterativeOptimization: WARN- Jacobian is rank deficient, rank = " << rank << ", rcond = " << rcond << "." << endl;
			cout << "Results may be inaccurate.  Try using IPOPT optimizer algorithm.\n" << endl;
		}

		// Compute temporary change in marker error
		for (int i = 0; i < getNumParameters(); i++)
		{
			_unprescribedQs[i]->coord->setValue(results[i]+dQ(i, 0), i==(getNumParameters()-1));
		}

		for (int i = 0; i < _markers.getSize(); i++)
		{
			if(!_markers[i]->validExperimentalPosition) continue;

			// Get marker offset in local frame
			Vec3 localPos;
			_markers[i]->marker->getOffset(localPos);

			// transform local marker to world frame
			Vec3 globalPos;
			de.transformPosition(*_markers[i]->body, localPos, globalPos);

			for (int r=0; r<3; r++)
			{
				_markers[i]->computedPosition[r] = globalPos[r];
				dError(i*3+r, 0) = sqrt(_markers[i]->weight) * (_markers[i]->experimentalPosition[r] - _markers[i]->computedPosition[r]); 
			}
		}

		// Q ERRORS
		int row = 3*_markers.getSize();
		for (int i = 0; i < getNumParameters(); i++) {
			if(_unprescribedQs[i]->weight) {
				////cout << _unprescribedQs[i]->coord->getName() << " w = " << _unprescribedQs[i]->weight << endl;
				dError(row, 0) = sqrt(_unprescribedQs[i]->weight) * (_unprescribedQs[i]->experimentalValue - _unprescribedQs[i]->coord->getValue());
				row++;
			}
		}

		tempDErrorNorm = dError.norm();

		// Check if temporary change in error exceeds the previous change in error
		if(tempDErrorNorm > previousDErrorNorm)
		{
			// Reduce the change in configuration by half until temporary change in error is less than the previous change in error
			while(tempDErrorNorm > previousDErrorNorm)
			{
			//	dError = dError*0.5;
			//	cout << "dQ reduced by a 10 percent" << "   dError.norm " << dError.norm() << "   dQ.norm " << dQ.norm() << endl;
				dQ = dQ*0.5;
				cout << "dQ reduced by a 50 percent" << "   dError.norm " << dError.norm() << "   dQ.norm " << dQ.norm() << endl;

				for (int i = 0; i < getNumParameters(); i++)
				{
					_unprescribedQs[i]->coord->setValue(results[i]+dQ(i, 0), i==(getNumParameters()-1));
				}

				for (int i = 0; i < _markers.getSize(); i++)
				{
					if(!_markers[i]->validExperimentalPosition) continue;

					// Get marker offset in local frame
					Vec3 localPos;
					_markers[i]->marker->getOffset(localPos);

					// transform local marker to world frame
					Vec3 globalPos;
					de.transformPosition(*_markers[i]->body, localPos, globalPos);

					for (int r=0; r<3; r++)
					{
						_markers[i]->computedPosition[r] = globalPos[r];
						dError(i*3+r, 0) = sqrt(_markers[i]->weight) * (_markers[i]->experimentalPosition[r] - _markers[i]->computedPosition[r]); 
					}
				}

				// Q ERRORS
				int row = 3*_markers.getSize();
				for (int i = 0; i < getNumParameters(); i++) {
					if(_unprescribedQs[i]->weight) {
						////cout << _unprescribedQs[i]->coord->getName() << " w = " << _unprescribedQs[i]->weight << endl;
						dError(row, 0) = sqrt(_unprescribedQs[i]->weight) * (_unprescribedQs[i]->experimentalValue - _unprescribedQs[i]->coord->getValue());
						row++;
					}
				}

				tempDErrorNorm = dError.norm();

			}
		}

		// Update error records
		currentDErrorNorm = dError.norm();
		deltaDErrorNorm = abs(currentDErrorNorm - previousDErrorNorm);
		
		// Make the change in configuration
		results.updCol(0) += dQ.col(0);

		//// Limit joint range of motion
		//for (int p=0; p<getNumParameters(); p++)
		//{
		//	if(results[p] >= _unprescribedQs[p]->coord->getRangeMin())
		//	{
		//		if(results[p] <= _unprescribedQs[p]->coord->getRangeMax()) { /* do nothing */ }
		//		else { results[p] = _unprescribedQs[p]->coord->getRangeMax(); cout << "MAX exceeded" << endl; }
		//	}
		//	else { results[p] = _unprescribedQs[p]->coord->getRangeMin(); cout << "MIN exceeded" << endl; }
		//}

		// Increment iteration
		iter++;

	}

	delete[] jpvt;

	// Assemble model in new configuration
	// x contains values only for unprescribed coordinates
	for (int i = 0; i < getNumParameters(); i++)
	{
		_unprescribedQs[i]->coord->setValue(results[i], i==(getNumParameters()-1));
	}

	// We keep track of worst marker for debugging/tuning purposes
	for (int i = 0; i < _markers.getSize(); i++)
	{
		if(!_markers[i]->validExperimentalPosition) continue;

		// Get marker offset in local frame
		Vec3 localPos;
		_markers[i]->marker->getOffset(localPos);

		// transform local marker to world frame
		Vec3 globalPos;
		de.transformPosition(*_markers[i]->body, localPos, globalPos);

		double markerError = 0.0;
		for (int j = 0; j < 3; j++)
		{
			double err = _markers[i]->experimentalPosition[j] - _markers[i]->computedPosition[j];
			markerError += (err * err);
		}

		totalMarkerSquaredErrors += markerError;
		if (markerError > maxMarkerError)
		{
			maxMarkerError = markerError;
			worstMarker = i;
		}

		totalWeightedSquaredErrors += _markers[i]->weight * markerError;

		if (debug)
			cout << _markers[i]->marker->getName() << " w = " << _markers[i]->weight 
				<< " exp = " << _markers[i]->experimentalPosition[0] << " " << _markers[i]->experimentalPosition[1] << " " << _markers[i]->experimentalPosition[2]
				<< " comp + " << globalPos[0] << " " << globalPos[1] << " " << globalPos[2] << endl;
	}

	for (int i = 0; i < _unprescribedWeightedQs.getSize(); i++)
	{
		double experimentalValue = _unprescribedWeightedQs[i]->experimentalValue;
		double computedValue = _unprescribedWeightedQs[i]->coord->getValue();
		double err = experimentalValue - computedValue;
		double coordinateError = err * err;

		totalCoordinateSquaredErrors += coordinateError;
		if (coordinateError > maxCoordinateError)
		{
			maxCoordinateError = coordinateError;
			worstCoordinate = i;
		}

		totalWeightedSquaredErrors += _unprescribedWeightedQs[i]->weight * coordinateError;

		if (debug)
			cout << _unprescribedWeightedQs[i]->coord->getName() << " w = " << _unprescribedWeightedQs[i]->weight << " exp = " << experimentalValue << " comp + " << computedValue << endl;
	}

	if (_printPerformanceValues || (!calcDerivs && debug))
	{
		cout << "total weighted squared error = " << totalWeightedSquaredErrors;
		if(totalMarkerSquaredErrors>0) {
			cout << ", marker error: RMS=" << sqrt(totalMarkerSquaredErrors/_markers.getSize());
			if (worstMarker >= 0) cout << ", max=" << sqrt(maxMarkerError) << " (" << _markers[worstMarker]->marker->getName() << ")";
		}
		if(totalCoordinateSquaredErrors>0) {
			cout << ", coord error: RMS=" << sqrt(totalCoordinateSquaredErrors/_unprescribedWeightedQs.getSize());
			if (worstCoordinate >= 0) cout << ", max=" << sqrt(maxCoordinateError) << " (" << _unprescribedWeightedQs[worstCoordinate]->coord->getName() << ")";
		}
		cout << endl;
		setErrorReportingQuantities(
			maxMarkerError, 
			(worstMarker<0)?"":_markers[worstMarker]->marker->getName(),
			maxCoordinateError, 
			(worstCoordinate<0)?"":_unprescribedWeightedQs[worstCoordinate]->coord->getName());
	}

	return 0;
}


//=============================================================================
// Helper methods for book keeping
//=============================================================================
//_____________________________________________________________________________
/**
 * prepareToSolve specifies the row of the Storage instance _experimentalDataStorage
 * that the optimizer is trying to solve.
 *
 * It also sets the values of the prescribed coordinates and returns the
 * initial guess for the unprescribed coordinates.
 */
void IKTarget::prepareToSolve(int aIndex, double* qGuess)
{
	double time;
	_experimentalDataStorage.getTime(aIndex,time);
	StateVector *dataRow = _experimentalDataStorage.getStateVector(aIndex);

	//--------------------------------------------------------------------
	// PRESCRIBED COORDINATES
	//--------------------------------------------------------------------
	// Set prescribed coordinates to their file value or to the constant experimental value
	for(int i=0; i<_prescribedQs.getSize(); i++)
	{
		double value;
		coordinateInfo *info = _prescribedQs[i];
		// Either get value from file or use the constantExperimentalValue
		if(_prescribedQs[i]->experimentalColumn >= 0)
			dataRow->getDataValue(info->experimentalColumn, value);
		else
			value = info->constantExperimentalValue;

		AbstractCoordinate *coord = info->coord;
		bool lockedState = coord->getLocked(); // presumebly this should return true since it's a prescribed Q!
		coord->setLocked(false);
		coord->setValue(value);
		coord->setLocked(lockedState);
	}

	//--------------------------------------------------------------------
	// UNPRESCRIBED COORDINATES
	//--------------------------------------------------------------------
	// Get initial guess and set the target experimental value for unprescribed coordinates
	for(int i=0; i<_unprescribedQs.getSize(); i++)
	{
		coordinateInfo *info = _unprescribedQs[i];

		// Set the initial guess
		if(info->experimentalColumn >= 0) {
			// Use the value from file as the initial guess
			dataRow->getDataValue(info->experimentalColumn, qGuess[i]);
		} else {
			// Use its current value as its initial guess
			qGuess[i] = info->coord->getValue();
		}

		// If this unprescribed coordinate has a nonzero weight, we need an experimental target value for it.
		// Get it either from file or based on the constantExperimentalValue
		if(info->weight)
			info->experimentalValue = (info->experimentalColumn >= 0) ? qGuess[i] : info->constantExperimentalValue;
	}

	//--------------------------------------------------------------------
	// MARKERS
	//--------------------------------------------------------------------
	// Get the experimental marker positions for all markers that will be solved (i.e. have non-zero weight)
	for(int i=0; i<_markers.getSize(); i++) {
		// get the location of the marker in the experimental data
		int dataColumnNumber = _markers[i]->experimentalColumn;

		dataRow->getDataValue(dataColumnNumber, _markers[i]->experimentalPosition[0]);
		dataRow->getDataValue(dataColumnNumber + 1, _markers[i]->experimentalPosition[1]);
		dataRow->getDataValue(dataColumnNumber + 2, _markers[i]->experimentalPosition[2]);

		/* If the marker is missing from this frame, its coordinates will
		 * all be NAN. In that case, do not compute an error for the marker.
		 * THIS IS COMPLETELY WRONG!! WHAT IF A MARKER POSITION IS SUPPOSED TO BE (0,0,0)??
		 * IT WOULD BE TRIGGERED AS INVALID!
		 */
		_markers[i]->validExperimentalPosition =
			!(rdMath::isNAN(_markers[i]->experimentalPosition[0]) ||
			 rdMath::isNAN(_markers[i]->experimentalPosition[1]) ||
			 rdMath::isNAN(_markers[i]->experimentalPosition[2]));
	}
}

//_____________________________________________________________________________
/**
 * buildMarkerNameMap is a utility used to construct an array of references to
 * markers that are in the model and also in the experimental data. Stored with
 * the reference is the corresponding index into the experimental data columns.
 */
void IKTarget::buildMarkerMap(const Array<string>& aNameArray)
{
	_markers.setSize(0);

	MarkerSet* markerSet = _model.getDynamicsEngine().getMarkerSet();

	for(int i=0; i<_ikTaskSet.getSize(); i++) {
		IKMarkerTask *markerTask = dynamic_cast<IKMarkerTask*>(_ikTaskSet.get(i));

		if(!markerTask || !markerTask->getApply()) continue; // not a marker task (or not being applied)

		string markerName=markerTask->getName();
		AbstractMarker *modelMarker = markerSet->get(markerName);
		if(!modelMarker)
			throw Exception("IKTarget.buildMarkerMap: ERROR- marker '"+markerName+
								 "' named in IKMarkerTask not found in model",__FILE__,__LINE__);

		if(markerTask->getWeight() == 0) continue; // we don't care about marker tasks with zero weight

		// Marker will have a _tx (and _ty, _tz) suffix in the storage file
		int j=aNameArray.findIndex(markerName+"_tx");
		if(j<0) 
			throw Exception("IKTarget.buildMarkerMap: ERROR- experimental data for marker '"+markerName+
								 "' not found in trc file",__FILE__,__LINE__);

		markerToSolve *newMarker = new markerToSolve;
		newMarker->marker = modelMarker;
		newMarker->body = modelMarker->getBody();
		newMarker->experimentalColumn = j - 1;		// make it j-1 to account for time column
		newMarker->weight = markerTask->getWeight();
		_markers.append(newMarker);
	}
}

//_____________________________________________________________________________
/**
 * buildCoordinateNameMap is a utility used to construct a map between an array of
 * coordinate names (aNameArray) and the coordinates in a SimmKinematicsEngine. The
 * size of the map is the number of coordinates in the SimmKinematicsEngine. It is
 * important that coordinates are not added or deleted after this map is made because
 * the indexing into the map depends on a fixed set of coordinates.
 */
void IKTarget::buildCoordinateMap(const Array<string>& aNameArray)
{
	CoordinateSet* coordinateSet = _model.getDynamicsEngine().getCoordinateSet();

	// Initialize info structures for all coordinates
	Array<coordinateInfo*> allCoordinates;
	for(int i=0; i<coordinateSet->getSize(); i++) {
		AbstractCoordinate *coord = coordinateSet->get(i);

		coordinateInfo *info = new coordinateInfo;
		info->coord = coord;
		info->prescribed = coord->getLocked() || coord->isConstrained();

		// Initialize as if it has no task
		info->experimentalColumn = -1;
		// initialize the constant experimental value (used if from_file is false) to the default value of the SimmCoordinate.
		// Previously was set to the current value of the coordinate, but that is less reliable when running from GUI.
		// If the IKCoordinateTask specifies its own value, constantExperimentalValue will be overwritten with that value below.
		info->constantExperimentalValue = coord->getDefaultValue();
		info->weight = 0;

		allCoordinates.append(info);
		//cout << i << "   " << info->coord->getName() << endl;
	}

	// Update info structures based on user-specified IKCoordinateTasks
	for(int i=0; i<_ikTaskSet.getSize(); i++) {
		IKCoordinateTask *coordTask = dynamic_cast<IKCoordinateTask*>(_ikTaskSet.get(i));

		if(!coordTask || !coordTask->getApply()) continue; // not a coordinate task

		string coordName = coordTask->getName();
		int coordIndex = coordinateSet->getIndex(coordName);
		if(coordIndex<0)
			throw Exception("IKTarget.buildCoordinateMap: ERROR- coordinate '"+coordName+
								 "' named in IKCoordinateTask not found in model",__FILE__,__LINE__);

		coordinateInfo *info = allCoordinates[coordIndex];

		// Potential issue here if marker has same name as coordinate...  We'll search in reverse
		// because coordinates should appear after markers in the storage.
		// NOTE: If we're not getting the experimental value from file, we'll use constantExperimentalValue
		if(coordTask->getValueType() == IKCoordinateTask::FromFile) {
			int j = aNameArray.rfindIndex(coordName);
			if(j < 0)
				throw Exception("IKTarget.buildCoordinateMap: ERROR- coordinate task '"+coordName+
									 "' specifies from_file but no column found for this coordinate in coordinates file",__FILE__,__LINE__);
			info->experimentalColumn = j - 1; // account for time column
		} else if(coordTask->getValueType() == IKCoordinateTask::ManualValue) {
			info->constantExperimentalValue = coordTask->getValue();
		}

		info->weight = coordTask->getWeight();
	}

	// Now we filter the coordinate infos into the three sets (not a partitioning since the second set is a subset of the first)
	_unprescribedQs.setSize(0);
	_unprescribedWeightedQs.setSize(0);
	_prescribedQs.setSize(0);

	for(int i=0; i<allCoordinates.getSize(); i++) {
		if(allCoordinates[i]->prescribed) _prescribedQs.append(allCoordinates[i]);
		else {
			_unprescribedQs.append(allCoordinates[i]);
			if(allCoordinates[i]->weight) _unprescribedWeightedQs.append(allCoordinates[i]);
		}
	}
}
//_____________________________________________________________________________
/**
 */
void IKTarget::printTasks() const
{
	if(_markers.getSize())
		cout << "Marker Tasks:" << endl;
	for(int i=0; i<_markers.getSize(); i++) {
		cout << "\t" << _markers[i]->marker->getName() << ": weight " << _markers[i]->weight;
		cout << " from file (columns " << _markers[i]->experimentalColumn << "-" << _markers[i]->experimentalColumn+2 << ")" << endl;
	}

	if(_unprescribedWeightedQs.getSize())
		cout << "Unprescribed Coordinate Tasks (with nonzero weight):" << endl;
	for(int i=0; i<_unprescribedWeightedQs.getSize(); i++) {
		cout << "\t" << _unprescribedWeightedQs[i]->coord->getName() << ": weight " << _unprescribedWeightedQs[i]->weight;
		if(_unprescribedWeightedQs[i]->experimentalColumn >= 0)
			cout << " from file (column " << _unprescribedWeightedQs[i]->experimentalColumn << ")" << endl;
		else
			cout << " constant target value of " << _unprescribedWeightedQs[i]->constantExperimentalValue << endl;
	}

	if(_prescribedQs.getSize())
		cout << "Prescribed Coordinate Tasks:" << endl;
	for(int i=0; i<_prescribedQs.getSize(); i++) {
		std::cout << "\t" << _prescribedQs[i]->coord->getName() << ": ";
		if(_prescribedQs[i]->experimentalColumn >= 0)
			cout << "from file (column " << _prescribedQs[i]->experimentalColumn << ")" << endl;
		else
			cout << "constant target value of " << _prescribedQs[i]->constantExperimentalValue << endl;
	}
}
//_____________________________________________________________________________
/**
 */
void IKTarget::printPerformance(double *x)
{
	_printPerformanceValues = true;
	double p;
	objectiveFunc(SimTK::Vector(getNumParameters(),x,true),true,p);
	_printPerformanceValues=false;
}

//_____________________________________________________________________________
/**
 * getComputedMarkerLocations returns current commputed (model) marker locations for debugging and display purposes
 */
void IKTarget::getComputedMarkerLocations(Array<double> &aMarkerLocations) const
{
	aMarkerLocations.setSize(0);
	for (int i = 0; i < _markers.getSize(); i++) 
		aMarkerLocations.append(3, &(_markers[i]->computedPosition)[0]);
}

//_____________________________________________________________________________
/**
 * getExperimentalMarkerLocations returns current experimental marker locations for debugging and display purposes
 */
void IKTarget::getExperimentalMarkerLocations(Array<double> &aMarkerLocations) const
{
	aMarkerLocations.setSize(0);
	for (int i = 0; i < _markers.getSize(); i++) 
		aMarkerLocations.append(3, &(_markers[i]->experimentalPosition)[0]);
}

//_____________________________________________________________________________
/**
 * getComputedMarkerLocations returns current marker locations for debugging and display purposes
 */
void IKTarget::getPrescribedCoordinateValues(Array<double>& aQValues) const
{
	aQValues.setSize(_prescribedQs.getSize());
	for (int i = 0; i < _prescribedQs.getSize(); i++)
		aQValues[i] = _prescribedQs.get(i)->coord->getValue();
}

void IKTarget::getUnprescribedCoordinateNames(Array<string>& aNameArray)
{
	aNameArray.setSize(_unprescribedQs.getSize());
	for (int i = 0; i < _unprescribedQs.getSize(); i++)
		aNameArray[i] = _unprescribedQs.get(i)->coord->getName();
}

void IKTarget::getPrescribedCoordinateNames(Array<string>& aNameArray)
{
	aNameArray.setSize(_prescribedQs.getSize());
	for (int i = 0; i < _prescribedQs.getSize(); i++)
		aNameArray[i] = _prescribedQs.get(i)->coord->getName();
}

void IKTarget::getOutputMarkerNames(Array<string>& aNameArray)
{
	aNameArray.setSize(_markers.getSize());
	for (int i = 0; i < _markers.getSize(); i++)
		aNameArray[i] = _markers[i]->marker->getName();
}

void IKTarget::setErrorReportingQuantities(const double& aMarkerError, const std::string& aMarkerName,
									const double& aCoordinateError, const std::string& aCoordinateName) const
{
	_worstMarkerError=aMarkerError;
	_nameOfWorstMarker=aMarkerName;
	_worstCoordinateError=aCoordinateError;
	_nameOfWorstCoordinate=aCoordinateName;
}


void IKTarget::createJacobian(const SimTK::Vector &jointQs, SimTK::Matrix &J)
{
	// Compute the Jacobian using central differences
	SimTK::Vector xp=jointQs.col(0);
	SimTK::Vector pf((3*_markers.getSize()+_unprescribedWeightedQs.getSize()));
	SimTK::Vector pb((3*_markers.getSize()+_unprescribedWeightedQs.getSize()));
	Vec3 globalPos;
	Vec3 localPos;
	SimTK::Real rdx;
	int row = 3*_markers.getSize();

	AbstractDynamicsEngine& de = _model.getDynamicsEngine();

	SimTK::Vector markerWeights(_markers.getSize());
	markerWeights = 0;
	for (int m=0; m<_markers.getSize(); m++)
	{
		if(!_markers[m]->validExperimentalPosition) continue;
		markerWeights(m) = sqrt(_markers[m]->weight);
	}

	pf = 0;
	pb = 0;

	// LOOP OVER CONTROLS
	for(int i=0;i<getNumParameters();i++) {

		//actualDX = 0.0;

		bool clampedState = _unprescribedQs[i]->coord->getClamped();
		_unprescribedQs[i]->coord->setClamped(false);

		// PERTURB FORWARD
		xp[i] = jointQs[i] + _dx[i];

		// Assemble model in new configuration
		// xp contains values only for unprescribed coordinates
		_unprescribedQs[i]->coord->setValue(xp[i], true);

		// Compute marker position in world frame
		for (int m=0; m<_markers.getSize(); m++)
		{
			if(!_markers[m]->validExperimentalPosition) continue;

			// Get marker offset in local frame
			_markers[m]->marker->getOffset(localPos);

			// transform local marker to world frame
			de.transformPosition(*_markers[m]->body, localPos, globalPos);

			for (int r=0; r<3; r++)
			{
                //pf(m*3+r) = markerWeights(m) * globalPos[r];
				pf(m*3+r) = markerWeights(m) * (globalPos[r] - _markers[m]->computedPosition[r]); // Forward difference only
			}
		}

		//// PERTURB BACKWARD
		//xp[i] = jointQs[i] - _dx[i];

		//// Assemble model in new configuration
		//// xp contains values only for unprescribed coordinates
		//_unprescribedQs[i]->coord->setValue(xp[i], true);

		//// Compute marker position in world frame	
		//for (int m=0; m<_markers.getSize(); m++)
		//{
		//	if(!_markers[m]->validExperimentalPosition) continue;

		//	// Get marker offset in local frame
		//	_markers[m]->marker->getOffset(localPos);

		//	// transform local marker to world frame
		//	de.transformPosition(*_markers[m]->body, localPos, globalPos);

		//	for (int r=0; r<3; r++)
		//	{
		//		pb(m*3+r) = markerWeights(m) * globalPos[r];
		//	}
		//}

		// DERIVATIVES OF PERFORMANCE
		//rdx = 0.5 / _dx[i];
		//J.updCol(i) = (rdx*(pf-pb));
		rdx = 1 / _dx[i];
		J.updCol(i) = (rdx*pf);

		// RESTORE CONTROLS
		xp[i] = jointQs[i];
		_unprescribedQs[i]->coord->setValue(xp[i], false);
		_unprescribedQs[i]->coord->setClamped(clampedState);

		// Q ERRORS
		if(_unprescribedQs[i]->weight) {
			J(row, i) = sqrt(_unprescribedQs[i]->weight);
			row++;
		}
	}
}


void IKTarget::createPseudoInverseJacobian(const SimTK::Matrix &J, SimTK::Matrix &Jinv)
{
	////Compute the pseudo-inverse of the Jacobian
	//Jinv = ((~J*J).invert())*(~J);

	char jobu, jobvt;
	int m, n, lda, ldu, ldvt, lwork, info;
 
	jobu = 'S';		// tell LAPACK the first min(m,n) columns of U are returned in the array U
	jobvt = 'S';	// tell LAPACK the first min(m,n) rows of VT are returned in the array VT
	m = (3*_markers.getSize()+_unprescribedWeightedQs.getSize());
	n = getNumParameters();
	lda = m;
	SimTK::Vector S(n);
	SimTK::Matrix Smat(n, n);
	SimTK::Matrix Sinv(n, n);
	SimTK::Matrix U(m, n);
	ldu = m;
	SimTK::Matrix VT(n, n);
	ldvt = n;
	SimTK::Vector WORK;
	lwork = max(3*min(m, n)+max(m, n), 5*min(m, n));
	WORK.resize(lwork);

	/* 
	   From SimTKlapack.h:

	   dgesvd_(SimTK_FOPT_(jobu), char *jobvt, 
	           SimTK_FDIM_(m), SimTK_FDIM_(n), double *a, SimTK_FDIM_(lda), 
	           double *s, double *u, SimTK_FDIM_(ldu), double *vt, SimTK_FDIM_(ldvt), 
			   double *work, SimTK_FDIM_(lwork), SimTK_INFO_, SimTK_FLEN_(jobu), SimTK_FLEN_(jobvt));
	*/

	SimTK::Matrix copyJ = J;
	dgesvd_(jobu, &jobvt, m, n, &copyJ(0,0), lda, &S(0), &U(0,0), ldu, &VT(0,0), ldvt, &WORK(0), lwork, info, 1, 1);

	Sinv = 0;

	for(int i=0; i<n; i++)
	{
		Sinv(i, i) = 1.0/S(i);
	}

	Jinv = ~VT*Sinv*~U;
}
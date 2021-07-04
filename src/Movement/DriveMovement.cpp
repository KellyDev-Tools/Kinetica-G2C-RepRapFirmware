/*
 * DriveMovement.cpp
 *
 *  Created on: 17 Jan 2015
 *      Author: David
 */

#include "DriveMovement.h"
#include "DDA.h"
#include "Move.h"
#include "StepTimer.h"
#include <Platform/RepRap.h>
#include <Math/Isqrt.h>
#include "Kinematics/LinearDeltaKinematics.h"

// Static members

DriveMovement *DriveMovement::freeList = nullptr;
unsigned int DriveMovement::numCreated = 0;

void DriveMovement::InitialAllocate(unsigned int num) noexcept
{
	while (num > numCreated)
	{
		freeList = new DriveMovement(freeList);
		++numCreated;
	}
}

// Allocate a DM, from the freelist if possible, else create a new one
DriveMovement *DriveMovement::Allocate(size_t p_drive, DMState st) noexcept
{
	DriveMovement * dm = freeList;
	if (dm != nullptr)
	{
		freeList = dm->nextDM;
		dm->nextDM = nullptr;
	}
	else
	{
		dm = new DriveMovement(nullptr);
		++numCreated;
	}
	dm->drive = (uint8_t)p_drive;
	dm->state = st;
	return dm;
}

// Constructors
DriveMovement::DriveMovement(DriveMovement *next) noexcept : nextDM(next)
{
}

// Non static members

void DriveMovement::DebugPrint() const noexcept
{
	const char c = (drive < reprap.GetGCodes().GetTotalAxes()) ? reprap.GetGCodes().GetAxisLetters()[drive] : (char)('0' + LogicalDriveToExtruder(drive));
	if (state != DMState::idle)
	{
		debugPrintf("DM%c%s dir=%c steps=%" PRIu32 " next=%" PRIu32 " rev=%" PRIu32 " interval=%" PRIu32 " psl=%" PRIu32 " A=%g B=%g C=%g ",
						c, (state == DMState::stepError) ? " ERR:" : ":", (direction) ? 'F' : 'B', totalSteps, nextStep, reverseStartStep, stepInterval, phaseStepLimit, (double)pA, (double)pB, (double)pC);
		if (isDelta)
		{
			debugPrintf("hmz0s=%.2f minusAaPlusBbTimesS=%.2f dSquaredMinusAsquaredMinusBsquared=%.2f drev=%.3f\n",
							(double)mp.delta.fHmz0s, (double)mp.delta.fMinusAaPlusBbTimesS, (double)mp.delta.fDSquaredMinusAsquaredMinusBsquaredTimesSsquared, (double)mp.delta.reverseStartDistance);
		}
		else
		{
			debugPrintf("pa=%.2f\n", (double)mp.cart.pressureAdvanceK);
		}
	}
	else
	{
		debugPrintf("DM%c: not moving\n", c);
	}
}

// This is called when currentSegment has just been changed to a new segment. Return true if there is a new segment to execute.
bool DriveMovement::NewCartesianSegment() noexcept
{
	while (true)
	{
		if (currentSegment == nullptr)
		{
			return false;
		}

		// Work out the movement limit in steps
		pC = currentSegment->CalcC(mp.cart.effectiveMmPerStep);
		if (currentSegment->IsLinear())
		{
			// Set up pB, pC such that for forward motion, time = pB + pC * stepNumber
			pB = currentSegment->CalcLinearB(distanceSoFar, timeSoFar);
			state = DMState::cartLinear;
		}
		else
		{
			// Set up pA, pB, pC such that for forward motion, time = pB + sqrt(pA + pC * stepNumber)
			pA = currentSegment->CalcNonlinearA(distanceSoFar);
			pB = currentSegment->CalcNonlinearB(timeSoFar);
			state = (currentSegment->IsAccelerating()) ? DMState::cartAccel : DMState::cartDecelNoReverse;
		}

		distanceSoFar += currentSegment->GetSegmentLength();
		timeSoFar += currentSegment->GetSegmentTime();

		phaseStepLimit = (uint32_t)(distanceSoFar * mp.cart.effectiveStepsPerMm) + 1;
		if (nextStep < phaseStepLimit)
		{
			return true;
		}

		currentSegment = currentSegment->GetNext();						// skip this segment
	}
}

// This is called when currentSegment has just been changed to a new segment. Return true if there is a new segment to execute.
bool DriveMovement::NewDeltaSegment(const DDA& dda) noexcept
{
	while (true)
	{
		if (currentSegment == nullptr)
		{
			return false;
		}

		const float stepsPerMm = reprap.GetPlatform().DriveStepsPerUnit(drive);
		pC = currentSegment->GetC()/stepsPerMm;		//TODO store the reciprocal to avoid the division
		if (currentSegment->IsLinear())
		{
			// Set up pB, pC such that for forward motion, time = pB + pC * (distanceMoved * steps/mm)
			pB = currentSegment->CalcLinearB(distanceSoFar, timeSoFar);
		}
		else
		{
			// Set up pA, pB, pC such that for forward motion, time = pB + sqrt(pA + pC * (distanceMoved * steps/mm))
			pA = currentSegment->CalcNonlinearA(distanceSoFar);
			pB = currentSegment->CalcNonlinearB(timeSoFar);
		}

		const float startDistance = distanceSoFar;
		distanceSoFar += currentSegment->GetSegmentLength();
		timeSoFar += currentSegment->GetSegmentTime();

		// Work out whether we reverse in this segment and the movement limit in steps
		const float sDx = distanceSoFar * dda.directionVector[0];
		const float sDy = distanceSoFar * dda.directionVector[1];
		const int32_t netStepsAtEnd = (int32_t)(fastSqrtf(mp.delta.fDSquaredMinusAsquaredMinusBsquaredTimesSsquared - fsquare(stepsPerMm) * (sDx * (sDx + mp.delta.fTwoA) + sDy * (sDy + mp.delta.fTwoB)))
								 	 	 	 	 + (distanceSoFar * dda.directionVector[2] - mp.delta.h0MinusZ0) * stepsPerMm);

		if (mp.delta.reverseStartDistance <= startDistance)
		{
			// This segment is purely downwards motion and we want the greater of the two quadratic solutions. There may have been upwards motion earlier in the move.
			if (direction)
			{
				direction = false;
				directionChanged = true;
			}
			state = DMState::deltaReverse;
			phaseStepLimit = (currentSegment->GetNext() == nullptr) ? totalSteps + 1
								: (reverseStartStep <= totalSteps) ? (uint32_t)((int32_t)(2 * reverseStartStep) - netStepsAtEnd)
									: 1 - netStepsAtEnd;
		}
		else if (distanceSoFar <= mp.delta.reverseStartDistance)
		{
			// This segment is purely upwards motion of the tower and we want the lower quadratic solution
			state = DMState::deltaForwardsNoReverse;
			phaseStepLimit = (currentSegment->GetNext() == nullptr) ? totalSteps + 1 : (uint32_t)(netStepsAtEnd + 1);
		}
		else
		{
			// This segment ends with reverse motion. We want the lower quadratic solution initially.
			phaseStepLimit = (currentSegment->GetNext() == nullptr) ? totalSteps + 1 : (uint32_t)((int32_t)(2 * reverseStartStep) - netStepsAtEnd);
			state = DMState::deltaForwardsReversing;
		}

		if (phaseStepLimit > nextStep)
		{
			return true;
		}

		currentSegment = currentSegment->GetNext();
	}
}

// This is called when currentSegment has just been changed to a new segment. Return true if there is a new segment to execute.
bool DriveMovement::NewExtruderSegment() noexcept
{
	while (true)
	{
		if (currentSegment == nullptr)
		{
			return false;
		}

		const float startDistance = distanceSoFar;
		const float startTime = timeSoFar;

		// Work out the movement limit in steps
		distanceSoFar += currentSegment->GetSegmentLength();
		timeSoFar += currentSegment->GetSegmentTime();

		pC = currentSegment->CalcC(mp.cart.effectiveMmPerStep);
		if (currentSegment->IsLinear())
		{
			// Set up pB, pC such that for forward motion, time = pB + pC * stepNumber
			pB = currentSegment->CalcLinearB(startDistance, startTime);
			phaseStepLimit = (uint32_t)(distanceSoFar * mp.cart.effectiveStepsPerMm) + 1;
			state = DMState::cartLinear;
		}
		else
		{
			// Set up pA, pB, pC such that for forward motion, time = pB + sqrt(pA + pC * stepNumber)
			pA = currentSegment->CalcNonlinearA(startDistance);
			pB = currentSegment->CalcNonlinearB(startTime, mp.cart.pressureAdvanceK);
			if (currentSegment->IsAccelerating())
			{
				// Extruders have a single acceleration segment. We need to add the extra extrusion distance due to pressure advance to the extrusion distance.
				distanceSoFar += mp.cart.extraExtrusionDistance;
				phaseStepLimit = (uint32_t)(distanceSoFar * mp.cart.effectiveStepsPerMm) + 1;
				state = DMState::cartAccel;
			}
			else
			{
				// This is a decelerating segment. If it includes pressure advance then it may include reversal.
				phaseStepLimit = totalSteps + 1;						// there is only one decelerating segment for extruders and it is at the end
				state = DMState::cartDecelForwardsReversing;			// assume that it may reverse
			}
		}

		if (nextStep < phaseStepLimit)
		{
			return true;
		}

		currentSegment = currentSegment->GetNext();						// skip this segment
	}
}

// Prepare this DM for a Cartesian axis move, returning true if there are steps to do
bool DriveMovement::PrepareCartesianAxis(const DDA& dda, const PrepParams& params) noexcept
{
	distanceSoFar = 0.0;
	timeSoFar = 0.0;
	mp.cart.pressureAdvanceK = 0.0;
	mp.cart.effectiveStepsPerMm = reprap.GetPlatform().DriveStepsPerUnit(drive) * fabsf(dda.directionVector[drive]);
	mp.cart.effectiveMmPerStep = 1.0/mp.cart.effectiveStepsPerMm;
	isDelta = false;
	isExtruder = false;
	currentSegment = (dda.shapedSegments != nullptr) ? dda.shapedSegments : dda.unshapedSegments;
	nextStep = 0;									// must do this before calling NewCartesianSegment

	if (!NewCartesianSegment())
	{
		return false;
	}

	// Prepare for the first step
	nextStepTime = 0;
	stepInterval = 999999;							// initialise to a large value so that we will calculate the time for just one step
	stepsTillRecalc = 0;							// so that we don't skip the calculation
	reverseStartStep = totalSteps + 1;				// no reverse phase
	return CalcNextStepTime(dda);
}

// Prepare this DM for a Delta axis move, returning true if there are steps to do
bool DriveMovement::PrepareDeltaAxis(const DDA& dda, const PrepParams& params) noexcept
{
	const float stepsPerMm = reprap.GetPlatform().DriveStepsPerUnit(drive);
	const float A = params.initialX - params.dparams->GetTowerX(drive);
	const float B = params.initialY - params.dparams->GetTowerY(drive);
	const float aAplusbB = A * dda.directionVector[X_AXIS] + B * dda.directionVector[Y_AXIS];
	const float dSquaredMinusAsquaredMinusBsquared = params.dparams->GetDiagonalSquared(drive) - fsquare(A) - fsquare(B);
	mp.delta.h0MinusZ0 = fastSqrtf(dSquaredMinusAsquaredMinusBsquared);
#if DM_USE_FPU
	mp.delta.fTwoA = 2.0 * A;
	mp.delta.fTwoB = 2.0 * B;
	mp.delta.fHmz0s = mp.delta.h0MinusZ0 * stepsPerMm;
	mp.delta.fMinusAaPlusBbTimesS = -(aAplusbB * stepsPerMm);
	mp.delta.fDSquaredMinusAsquaredMinusBsquaredTimesSsquared = dSquaredMinusAsquaredMinusBsquared * fsquare(stepsPerMm);
#else
	qq;	// incomplete!
	mp.delta.hmz0sK = roundS32(h0MinusZ0 * stepsPerMm * DriveMovement::K2);
	mp.delta.minusAaPlusBbTimesKs = -roundS32(aAplusbB * stepsPerMm * DriveMovement::K2);
	mp.delta.dSquaredMinusAsquaredMinusBsquaredTimesKsquaredSsquared = roundS64(dSquaredMinusAsquaredMinusBsquared * fsquare(stepsPerMm * DriveMovement::K2));
#endif

	// Calculate the distance at which we need to reverse direction.
	if (params.a2plusb2 <= 0.0)
	{
		// Pure Z movement. We can't use the main calculation because it divides by a2plusb2.
		direction = (dda.directionVector[Z_AXIS] >= 0.0);
		mp.delta.reverseStartDistance = (direction) ? dda.totalDistance + 1.0 : -1.0;	// so that we never reverse and NewDeltaSegment knows which way we are going
		reverseStartStep = totalSteps + 1;
	}
	else
	{
		// The distance to reversal is the solution to a quadratic equation. One root corresponds to the carriages being below the bed,
		// the other root corresponds to the carriages being above the bed.
		const float drev = ((dda.directionVector[Z_AXIS] * fastSqrtf(params.a2plusb2 * params.dparams->GetDiagonalSquared(drive) - fsquare(A * dda.directionVector[Y_AXIS] - B * dda.directionVector[X_AXIS])))
							- aAplusbB)/params.a2plusb2;
		mp.delta.reverseStartDistance = drev;
		if (drev > 0.0 && drev < dda.totalDistance)					// if the reversal point is within range
		{
			// Calculate how many steps we need to move up before reversing
			const float hrev = dda.directionVector[Z_AXIS] * drev + fastSqrtf(dSquaredMinusAsquaredMinusBsquared - 2 * drev * aAplusbB - params.a2plusb2 * fsquare(drev));
			const int32_t numStepsUp = (int32_t)((hrev - mp.delta.h0MinusZ0) * stepsPerMm);

			// We may be almost at the peak height already, in which case we don't really have a reversal.
			if (numStepsUp < 1)
			{
				mp.delta.reverseStartDistance = -1.0;				// so that we know we have reversed already
				reverseStartStep = totalSteps + 1;
				direction = false;
			}
			else
			{
				reverseStartStep = (uint32_t)numStepsUp + 1;

				// Correct the initial direction and the total number of steps
				if (direction)
				{
					// Net movement is up, so we will go up first and then down by a lesser amount
					totalSteps = (2 * numStepsUp) - totalSteps;
				}
				else
				{
					// Net movement is down, so we will go up first and then down by a greater amount
					direction = true;
					totalSteps = (2 * numStepsUp) + totalSteps;
				}
			}
		}
		else
		{
			// No reversal
			reverseStartStep = totalSteps + 1;
			direction = (drev >= 0.0);
		}
	}

	distanceSoFar = 0.0;
	timeSoFar = 0.0;
	isDelta = true;
	currentSegment = (dda.shapedSegments != nullptr) ? dda.shapedSegments : dda.unshapedSegments;

	nextStep = 0;									// must do this before calling NewDeltaSegment
	if (!NewDeltaSegment(dda))
	{
		return false;
	}

	// Prepare for the first step
	nextStepTime = 0;
	stepInterval = 999999;							// initialise to a large value so that we will calculate the time for just one step
	stepsTillRecalc = 0;							// so that we don't skip the calculation
	return CalcNextStepTime(dda);
}

// Prepare this DM for an extruder move, returning true if there are steps to do
// We have already generated the extruder segments and we know that there are some
bool DriveMovement::PrepareExtruder(const DDA& dda, const PrepParams& params) noexcept
{
	ExtruderShaper& shaper = reprap.GetMove().GetExtruderShaper(LogicalDriveToExtruder(drive));
	distanceSoFar = shaper.GetExtrusionPending()/dda.directionVector[drive];

	const float stepsPerMm = reprap.GetPlatform().DriveStepsPerUnit(drive);
	mp.cart.effectiveStepsPerMm = stepsPerMm * fabsf(dda.directionVector[drive]);
	mp.cart.effectiveMmPerStep = 1.0/mp.cart.effectiveStepsPerMm;

	// Calculate the total forward and reverse movement distances
	float forwardDistance = distanceSoFar;
	float reverseDistance;

	if (dda.flags.usePressureAdvance && shaper.GetK() > 0.0)
	{
		// We are using nonzero pressure advance. Movement must be forwards.
		mp.cart.pressureAdvanceK = shaper.GetK();
		mp.cart.extraExtrusionDistance = mp.cart.pressureAdvanceK * dda.acceleration * params.accelClocks;	//TODO use the last speed at which we did any real extrusion instead
		forwardDistance += mp.cart.extraExtrusionDistance;

		// Check if there is a reversal in the deceleration segment
		const MoveSegment * const decelSeg = dda.unshapedSegments->GetFirstDecelSegment();
		if (decelSeg == nullptr)
		{
			forwardDistance += dda.totalDistance;
			reverseDistance = 0.0;
		}
		else
		{
			const float initialDecelSpeed = dda.topSpeed - mp.cart.pressureAdvanceK * dda.deceleration;
			if (initialDecelSpeed <= 0.0)
			{
				// The entire deceleration segment is in reverse
				forwardDistance += params.decelStartDistance;
				reverseDistance = ((0.5 * dda.deceleration * params.decelClocks) - initialDecelSpeed) * params.decelClocks;
			}
			else
			{
				const float timeToReverse = initialDecelSpeed * ((-0.5) * decelSeg->GetC());	// 'c' is -2/deceleration, so -0.5*c is 1/deceleration
				if (timeToReverse < params.decelClocks)
				{
					// There is a reversal
					const float distanceToReverse = 0.5 * dda.deceleration * fsquare(timeToReverse);
					forwardDistance += params.decelStartDistance + distanceToReverse;
					reverseDistance = 0.5 * dda.deceleration * fsquare(params.decelClocks - timeToReverse);
				}
				else
				{
					// No reversal
					forwardDistance += dda.totalDistance - (mp.cart.pressureAdvanceK * dda.deceleration * params.decelClocks);
					reverseDistance = 0.0;
				}
			}
		}
	}
	else
	{
		// No pressure advance. Movement may be backwards but this still counts as forward distance in the calculations.
		mp.cart.pressureAdvanceK = mp.cart.extraExtrusionDistance = 0.0;
		forwardDistance += dda.totalDistance;
		reverseDistance = 0.0;
	}

	// Check whether there are any steps at all
	const float forwardSteps = forwardDistance * mp.cart.effectiveStepsPerMm;
	if (reverseDistance > 0.0)
	{
		const float netDistance = forwardDistance - reverseDistance;
		const int32_t netSteps = netDistance * mp.cart.effectiveStepsPerMm;
		if (netSteps == 0 && forwardSteps <= 1)
		{
			// No movement at all, or one step forward and one step back which we will ignore
			shaper.SetExtrusionPending(netDistance * dda.directionVector[drive]);
			return false;
		}

		reverseStartStep = forwardSteps + 1;
		totalSteps = 2 * reverseStartStep - forwardSteps;
		shaper.SetExtrusionPending((netDistance - (float)netSteps * mp.cart.effectiveMmPerStep) * dda.directionVector[drive]);
	}
	else
	{
		if (forwardSteps >= 1.0)
		{
			totalSteps = (uint32_t)forwardSteps;
			shaper.SetExtrusionPending((forwardDistance - (float)totalSteps * mp.cart.effectiveMmPerStep) * dda.directionVector[drive]);
		}
		else if (forwardSteps <= -1.0)
		{
			totalSteps = (uint32_t)(-forwardSteps);
			shaper.SetExtrusionPending((forwardDistance + (float)totalSteps * mp.cart.effectiveMmPerStep) * dda.directionVector[drive]);
		}
		else
		{
			shaper.SetExtrusionPending(forwardDistance * dda.directionVector[drive]);
			return false;
		}
		reverseStartStep = totalSteps + 1;			// no reverse phase
	}

	currentSegment = dda.unshapedSegments;
	timeSoFar = 0.0;
	isDelta = false;
	isExtruder = true;

	nextStep = 0;									// must do this before calling NewExtruderSegment
	if (!NewExtruderSegment())
	{
		return false;								// this should not happen because we have already determined that there are steps to do
	}

	// Prepare for the first step
	nextStepTime = 0;
	stepInterval = 999999;							// initialise to a large value so that we will calculate the time for just one step
	stepsTillRecalc = 0;							// so that we don't skip the calculation
	return CalcNextStepTime(dda);
}

// Calculate and store the time since the start of the move when the next step for the specified DriveMovement is due.
// We have already incremented nextStep and checked that it does not exceed totalSteps, so at least one more step is due
// Return true if all OK, false to abort this move because the calculation has gone wrong
bool DriveMovement::CalcNextStepTimeFull(const DDA &dda) noexcept
pre(nextStep <= totalSteps; stepsTillRecalc == 0)
{
	uint32_t stepsToLimit = phaseStepLimit - nextStep;

	// If there are no more steps left in this segment, skip to the next segment
	if (stepsToLimit == 0)
	{
		currentSegment = currentSegment->GetNext();
		const bool more = (isDelta) ? NewDeltaSegment(dda)
							: (isExtruder) ? NewExtruderSegment()
								: NewCartesianSegment();
		if (!more)
		{
			state = DMState::stepError;
			nextStep += 100000000;								// so we can tell what happened in the debug print
			return false;
		}
	}

	if (phaseStepLimit > reverseStartStep)
	{
		stepsToLimit = reverseStartStep - nextStep;
	}

	uint32_t shiftFactor = 0;									// assume single stepping
	if (stepsToLimit > 1 && stepInterval < DDA::MinCalcInterval)
	{
		if (stepInterval < DDA::MinCalcInterval/4 && stepsToLimit > 8)
		{
			shiftFactor = 3;									// octal stepping
		}
		else if (stepInterval < DDA::MinCalcInterval/2 && stepsToLimit > 4)
		{
			shiftFactor = 2;									// quad stepping
		}
		else if (stepsToLimit > 2)
		{
			shiftFactor = 1;									// double stepping
		}
	}
	stepsTillRecalc = (1u << shiftFactor) - 1u;					// store number of additional steps to generate

	uint32_t nextCalcStepTime;

	// Work out the time of the step
	switch (state)
	{
	case DMState::cartLinear:									// linear steady speed
		nextCalcStepTime = pB + (float)(nextStep + stepsTillRecalc) * pC;
		break;

	case DMState::cartAccel:									// Cartesian accelerating
		nextCalcStepTime = pB + fastSqrtf(pA + pC * (float)(nextStep + stepsTillRecalc));
		break;

	case DMState::cartDecelForwardsReversing:
		if (nextStep <= reverseStartStep)
		{
			nextCalcStepTime = pB - fastSqrtf(pA + pC * (float)(nextStep + stepsTillRecalc));
			break;
		}

		direction = false;
		directionChanged = true;
		state = DMState::cartDecelReverse;
		// no break
	case DMState::cartDecelReverse:								// Cartesian decelerating, reverse motion
		nextCalcStepTime = pB + fastSqrtf(pA + pC * (float)((2 * reverseStartStep - nextStep) + stepsTillRecalc));
		break;

	case DMState::cartDecelNoReverse:							// Cartesian accelerating with no reversal
		nextCalcStepTime = pB - fastSqrtf(pA + pC * (float)(nextStep + stepsTillRecalc));
		break;

	case DMState::deltaForwardsReversing:						// moving forwards
		if (nextStep == reverseStartStep)
		{
			direction = false;
			directionChanged = true;
			state = DMState::deltaReverse;
		}
		// no break
	case DMState::deltaForwardsNoReverse:
	case DMState::deltaReverse:									// reversing on this and subsequent steps
		// Calculate d*s where d = distance the head has travelled, s = steps/mm for this drive
		{
			const float steps = (float)(1u << shiftFactor);
			if (direction)
			{
				mp.delta.fHmz0s += steps;						// get new carriage height above Z in steps
			}
			else
			{
				mp.delta.fHmz0s -= steps;						// get new carriage height above Z in steps
			}

			const float hmz0sc = mp.delta.fHmz0s * dda.directionVector[Z_AXIS];
			const float t1 = mp.delta.fMinusAaPlusBbTimesS + hmz0sc;
			const float t2a = mp.delta.fDSquaredMinusAsquaredMinusBsquaredTimesSsquared - fsquare(mp.delta.fHmz0s) + fsquare(t1);
			// Due to rounding error we can end up trying to take the square root of a negative number if we do not take precautions here
			const float t2 = (t2a > 0.0) ? fastSqrtf(t2a) : 0.0;
			const float ds = (direction) ? t1 - t2 : t1 + t2;

			// Now feed ds into the step algorithm for Cartesian motion
			if (ds < 0.0)
			{
				state = DMState::stepError;
				nextStep += 110000000;							// so that we can tell what happened in the debug print
				return false;
			}

			const float pCds = pC * ds;
			nextCalcStepTime = (currentSegment->IsLinear()) ? pB + pCds
								: (currentSegment->IsAccelerating()) ? pB + fastSqrtf(pA + pCds)
									 : pB - fastSqrtf(pA + pCds);
			if (currentSegment->IsLinear()) { pA = ds; }	//DEBUG
		}
		break;

	default:
		return false;
	}

	// When crossing between movement phases with high microstepping, due to rounding errors the next step may appear to be due before the last one
	stepInterval = (nextCalcStepTime > nextStepTime)
					? (nextCalcStepTime - nextStepTime) >> shiftFactor	// calculate the time per step, ready for next time
					: 0;
#if EVEN_STEPS
	nextStepTime = nextCalcStepTime - (stepsTillRecalc * stepInterval);
#else
	nextStepTime = nextCalcStepTime;
#endif

	if (nextCalcStepTime > dda.clocksNeeded)
	{
		// The calculation makes this step late.
		// When the end speed is very low, calculating the time of the last step is very sensitive to rounding error.
		// So if this is the last step and it is late, bring it forward to the expected finish time.
		// Very rarely on a delta, the penultimate step may also be calculated late. Allow for that here in case it affects Cartesian axes too.
		if (nextStep + 1 >= totalSteps)
		{
			nextStepTime = dda.clocksNeeded;
		}
		else
		{
			// We don't expect any step except the last to be late
			state = DMState::stepError;
			nextStep += 120000000;							// so we can tell what happened in the debug print
			stepInterval = nextCalcStepTime;	//DEBUG
			return false;
		}
	}

	return true;
}

// End

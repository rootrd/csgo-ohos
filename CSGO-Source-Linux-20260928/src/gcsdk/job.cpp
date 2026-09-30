//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: Job type registration and the CJob object state. Jobs are created
//			only when a GC connection routes a message to them; this library
//			has no GC connection, so no job scheduler or coroutine is provided
//			and yielding reports failure instead of suspending.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include "tier0/memdbgon.h"

namespace GCSDK
{

CJob *g_pJobCur = NULL;
bool CJob::s_bStartDefaultJobsDelayed = true;
uint64 CJobTime::sm_lTimeCur = 0;

CJobTime::CJobTime()
	: m_lTime( sm_lTimeCur )
{
}

static CUtlVector< const JobType_t * > &JobTypeRegistry()
{
	// Registration runs from static initializers across many translation units.
	static CUtlVector< const JobType_t * > s_vecJobTypes;
	return s_vecJobTypes;
}

void CJobMgr::RegisterJobType( const JobType_t *pJobType )
{
	if ( !pJobType )
		return;
	CUtlVector< const JobType_t * > &registry = JobTypeRegistry();
	for ( int i = 0; i < registry.Count(); ++i )
	{
		if ( registry[i] == pJobType )
			return;
		if ( pJobType->m_eCreationMsg != 0 && registry[i]->m_eCreationMsg == pJobType->m_eCreationMsg )
		{
			AssertMsg2( false, "Jobs %s and %s both handle the same GC message", registry[i]->m_pchName, pJobType->m_pchName );
		}
	}
	registry.AddToTail( pJobType );
}

CJob::CJob( CJobMgr &jobMgr, char const *pchJobName )
	: m_JobMgr( jobMgr )
{
	m_bRunFromMsg = 0;
	m_bWorkItemCanceled = 0;
	m_bIsTest = 0;
	m_bIsLongRunning = 0;
	m_JobID = k_GIDNil;
	m_hCoroutine = HCoroutine();
	m_pvStartParam = NULL;
	m_flags.m_uFlags = 0;
	m_cLocksAttempted = 0;
	m_cLocksWaitedFor = 0;
	m_ePauseReason = k_EJobPauseReasonNotStarted;
	m_pszPauseResourceName = NULL;
	m_unWaitMsgType = 0;
	m_nContextMask = 0;
	m_pJobPrev = NULL;
	m_pWaitingOnLock = NULL;
	m_pWaitingOnLockFilename = NULL;
	m_waitingOnLockLine = 0;
	m_pJobToNotifyOnLockRelease = NULL;
	m_pWaitingOnWorkItem = NULL;
	m_jobIDWaitingOn = k_GIDNil;
	m_pFirstJobWaitingOnThis = NULL;
	m_pJobWaitingOnNext = NULL;
	m_pJobType = NULL;
	m_pchJobName = pchJobName ? pchJobName : "";
}

CJob::~CJob()
{
	AssertMsg1( m_vecNetPackets.Count() == 0, "Job %s destroyed while holding network packets", GetName() );
}

const char *CJob::GetName() const
{
	return m_pJobType ? m_pJobType->m_pchName : m_pchJobName;
}

uint32 CJob::CHeartbeatsBeforeTimeout()
{
	return k_cJobHeartbeatsBeforeTimeoutDefault;
}

bool CJob::BYieldingWaitOneFrame()
{
	AssertMsg1( false, "Job %s tried to yield without a job scheduler", GetName() );
	return false;
}

} // namespace GCSDK

/*
 *	Author: bwilliams
 *  Created: Oct 20, 2011
 *
 *	Copyright (c) 2011 Vmware, Inc.  All rights reserved.
 *	-- VMware Confidential
 */

#ifndef CSimpleAsyncTaskExecutorState_h_
#define CSimpleAsyncTaskExecutorState_h_

namespace Caf {

class INTEGRATIONCORE_LINKAGE CSimpleAsyncTaskExecutorState {
public:
	CSimpleAsyncTaskExecutorState();
	virtual ~CSimpleAsyncTaskExecutorState();

public:
	void initialize(
		const SmartPtrIRunnable& runnable,
		const SmartPtrIErrorHandler& errorHandler);

	SmartPtrIRunnable getRunnable() const;
	SmartPtrIErrorHandler getErrorHandler() const;
	ITaskExecutor::ETaskState getState() const;
	std::string getStateStr() const;

	void setState(const ITaskExecutor::ETaskState runnableState);

	void signalStart();
	void waitForStart(SmartPtrCAutoMutex& mutex, const uint32 timeoutMs);

	void signalStop();
	void waitForStop(SmartPtrCAutoMutex& mutex, const uint32 timeoutMs);

	void detach();

private:
	bool _isInitialized;
	ITaskExecutor::ETaskState _runnableState;
	SmartPtrIRunnable _runnable;
	SmartPtrIErrorHandler _errorHandler;
	std::string _exceptionMessage;

	CThreadSignal _threadSignalStart;
	CThreadSignal _threadSignalStop;

private:
	CAF_CM_CREATE;
	CAF_CM_CREATE_LOG;
	CAF_CM_CREATE_THREADSAFE;
	CAF_CM_DECLARE_NOCOPY(CSimpleAsyncTaskExecutorState);
};

CAF_DECLARE_SMART_POINTER(CSimpleAsyncTaskExecutorState);

}

#endif // #ifndef CSimpleAsyncTaskExecutorState_h_
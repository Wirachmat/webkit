/*
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2013 Apple Inc. All Rights Reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "JSEventListener.h"

#include "BeforeUnloadEvent.h"
#include "Event.h"
#include "Frame.h"
#include "JSEvent.h"
#include "JSEventTarget.h"
#include "JSMainThreadExecState.h"
#include "JSMainThreadExecStateInstrumentation.h"
#include "ScriptController.h"
#include "WorkerGlobalScope.h"
#include <runtime/ExceptionHelpers.h>
#include <runtime/JSLock.h>
#include <runtime/VMEntryScope.h>
#include <wtf/Ref.h>
#include <wtf/RefCountedLeakCounter.h>

using namespace JSC;

namespace WebCore {

JSEventListener::JSEventListener(JSObject* function, JSObject* wrapper, bool isAttribute, DOMWrapperWorld& isolatedWorld)
    : EventListener(JSEventListenerType)
    , m_wrapper(wrapper)
    , m_isAttribute(isAttribute)
    , m_isolatedWorld(&isolatedWorld)
{
    if (wrapper) {
        JSC::Heap::heap(wrapper)->writeBarrier(wrapper, function);
        m_jsFunction = JSC::Weak<JSC::JSObject>(function);
    } else
        ASSERT(!function);
}

JSEventListener::~JSEventListener()
{
}

JSObject* JSEventListener::initializeJSFunction(ScriptExecutionContext*) const
{
    return 0;
}

void JSEventListener::visitJSFunction(SlotVisitor& visitor)
{
    // If m_wrapper is 0, then m_jsFunction is zombied, and should never be accessed.
    if (!m_wrapper)
        return;

    visitor.appendUnbarrieredWeak(&m_jsFunction);
}

void JSEventListener::handleEvent(ScriptExecutionContext* scriptExecutionContext, Event* event)
{
    ASSERT(scriptExecutionContext);
    if (!scriptExecutionContext || scriptExecutionContext->isJSExecutionForbidden())
        return;

    JSLockHolder lock(scriptExecutionContext->vm());

    JSObject* jsFunction = this->jsFunction(scriptExecutionContext);
    if (!jsFunction)
        return;

    JSDOMGlobalObject* globalObject = toJSDOMGlobalObject(scriptExecutionContext, *m_isolatedWorld);
    if (!globalObject)
        return;

    if (scriptExecutionContext->isDocument()) {
        JSDOMWindow* window = jsCast<JSDOMWindow*>(globalObject);
        if (!window->impl().isCurrentlyDisplayedInFrame())
            return;
        // FIXME: Is this check needed for other contexts?
        ScriptController& script = window->impl().frame()->script();
        if (!script.canExecuteScripts(AboutToExecuteScript) || script.isPaused())
            return;
    }

    ExecState* exec = globalObject->globalExec();
    JSValue handleEventFunction = jsFunction;

    CallData callData;
    CallType callType = getCallData(handleEventFunction, callData);
    // If jsFunction is not actually a function, see if it implements the EventListener interface and use that
    if (callType == CallTypeNone) {
        handleEventFunction = jsFunction->get(exec, Identifier(exec, "handleEvent"));
        callType = getCallData(handleEventFunction, callData);
    }

    if (callType != CallTypeNone) {
        Ref<JSEventListener> protect(*this);

        MarkedArgumentBuffer args;
        args.append(toJS(exec, globalObject, event));

        Event* savedEvent = globalObject->currentEvent();
        globalObject->setCurrentEvent(event);

        VM& vm = globalObject->vm();
        VMEntryScope entryScope(vm, vm.entryScope ? vm.entryScope->globalObject() : globalObject);

        InspectorInstrumentationCookie cookie = JSMainThreadExecState::instrumentFunctionCall(scriptExecutionContext, callType, callData);

        JSValue thisValue = handleEventFunction == jsFunction ? toJS(exec, globalObject, event->currentTarget()) : jsFunction;
        JSValue exception;
        JSValue retval = scriptExecutionContext->isDocument()
            ? JSMainThreadExecState::call(exec, handleEventFunction, callType, callData, thisValue, args, &exception)
            : JSC::call(exec, handleEventFunction, callType, callData, thisValue, args, &exception);

        InspectorInstrumentation::didCallFunction(cookie, scriptExecutionContext);

        globalObject->setCurrentEvent(savedEvent);

        if (is<WorkerGlobalScope>(*scriptExecutionContext)) {
            bool terminatorCausedException = (exec->hadException() && isTerminatedExecutionException(exec->exception()));
            if (terminatorCausedException || (vm.watchdog && vm.watchdog->didFire()))
                downcast<WorkerGlobalScope>(*scriptExecutionContext).script()->forbidExecution();
        }

        if (exception) {
            event->target()->uncaughtExceptionInEventHandler();
            reportException(exec, exception);
        } else {
            if (!retval.isUndefinedOrNull() && is<BeforeUnloadEvent>(*event))
                downcast<BeforeUnloadEvent>(*event).setReturnValue(retval.toString(exec)->value(exec));
            if (m_isAttribute) {
                if (retval.isFalse())
                    event->preventDefault();
            }
        }
    }
}

bool JSEventListener::virtualisAttribute() const
{
    return m_isAttribute;
}

bool JSEventListener::operator==(const EventListener& listener)
{
    if (const JSEventListener* jsEventListener = JSEventListener::cast(&listener))
        return m_jsFunction == jsEventListener->m_jsFunction && m_isAttribute == jsEventListener->m_isAttribute;
    return false;
}

Ref<JSEventListener> createJSEventListenerForAdd(JSC::ExecState& state, JSC::JSObject& listener, JSC::JSObject& wrapper)
{
    // FIXME: This abstraction is no longer needed. It was part of support for SVGElementInstance.
    // We should remove it and simplify the bindings generation scripts.
    return JSEventListener::create(&listener, &wrapper, false, currentWorld(&state));
}

} // namespace WebCore

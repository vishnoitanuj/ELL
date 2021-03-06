////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Learning Library (ELL)
//  File:     IRRuntime.cpp (emitters)
//  Authors:  Umesh Madan, Chuck Jacobs
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "IRRuntime.h"
#include "IRFunctionEmitter.h"
#include "IRMetadata.h"
#include "IRModuleEmitter.h"

// utilities
#include "Unused.h"

namespace ell
{
namespace emitters
{
    namespace
    {
        //
        // Native implementations of matrix operation functions (as opposed to calling out to BLAS)
        //
        template <typename ValueType>
        LLVMFunction EmitGEMVFunction(IRModuleEmitter& module, const std::string& functionName, const VariableTypeList& argTypes)
        {
            auto function = module.BeginFunction(functionName, VariableType::Int32, argTypes);
            auto arguments = function.Arguments().begin();
            auto order = &(*arguments++);
            auto transpose = &(*arguments++);
            auto m = function.LocalScalar(&(*arguments++));
            auto n = function.LocalScalar(&(*arguments++));
            auto alpha = function.LocalScalar(&(*arguments++));
            auto A = function.LocalArray(&(*arguments++));
            auto lda = function.LocalScalar(&(*arguments++));
            auto x = function.LocalArray(&(*arguments++));
            auto incx = function.LocalScalar(&(*arguments++));
            auto beta = function.LocalScalar(&(*arguments++));
            auto y = function.LocalArray(&(*arguments++));
            auto incy = function.LocalScalar(&(*arguments++));
            UNUSED(order, transpose, alpha, beta);

            LLVMValue accum = function.Variable(emitters::GetVariableType<ValueType>(), "accum");

            function.For(m, [A, x, y, incx, incy, lda, n, accum](IRFunctionEmitter& function, auto rowIndex) {
                function.StoreZero(accum);
                function.For(n, [rowIndex, A, x, incx, lda, accum](IRFunctionEmitter& function, auto columnIndex) {
                    auto aIndex = (rowIndex*lda) + columnIndex;
                    auto xIndex = columnIndex * incx;
                    auto aVal = A[aIndex];
                    auto xVal = x[xIndex];
                    auto aTimesX = aVal * xVal;
                    function.Store(accum, function.Load(accum) + aTimesX);
                });

                auto yIndex = rowIndex * incy;
                y[yIndex] = function.Load(accum);
            });

            function.Return(function.Literal<int>(0));
            module.EndFunction();
            return function.GetFunction();
        }

        template <typename ValueType>
        LLVMFunction EmitGEMMFunction(IRModuleEmitter& module, const std::string& functionName, const VariableTypeList& argTypes)
        {
            const auto CblasNoTrans = 111;
            const auto CblasTrans = 112;

            auto function = module.BeginFunction(functionName, VariableType::Int32, argTypes);
            auto arguments = function.Arguments().begin();
            auto order = &(*arguments++);
            auto transposeA = function.LocalScalar(&(*arguments++)) == CblasTrans;
            auto transposeB = function.LocalScalar(&(*arguments++)) == CblasTrans;
            auto m = function.LocalScalar(&(*arguments++));
            auto n = function.LocalScalar(&(*arguments++));
            auto k = function.LocalScalar(&(*arguments++));
            auto alpha = function.LocalScalar(&(*arguments++));
            auto A = function.LocalArray(&(*arguments++));
            auto lda = function.LocalScalar(&(*arguments++));
            auto B = function.LocalArray(&(*arguments++));
            auto ldb = function.LocalScalar(&(*arguments++));
            auto beta = function.LocalScalar(&(*arguments++));
            auto C = function.LocalArray(&(*arguments++));
            auto ldc = function.LocalScalar(&(*arguments++));
            UNUSED(CblasNoTrans, order, alpha, beta);

            // C = A x B, A: mxk, B: kxn, C: mxn
            // A': kxm, B': nxk

            // Loop orders:
            // A*B:   i, k, j
            // A'*B:  k, i, j
            // A*B':  i, j, k
            // A'*B': k, j, i (?)

            // Clear output
            auto count = ldc * m;
            function.MemorySet<ValueType>(C, function.Literal<int>(0), function.Literal<uint8_t>(0), count);

            // Accumulate partial values into output
            function.For(m, [A, B, C, lda, ldb, ldc, transposeA, transposeB, n, k](IRFunctionEmitter& function, auto i) {
                function.For(k, [i, A, B, C, lda, ldb, ldc, transposeA, transposeB, n](IRFunctionEmitter& function, auto k) {
                    function.For(n, [i, k, A, B, C, lda, ldb, ldc, transposeA, transposeB](IRFunctionEmitter& function, auto j) {
                        auto aOffset = function.LocalScalar(function.Select(transposeA, (k * lda) + i, (i * lda) + k));
                        auto bOffset = function.LocalScalar(function.Select(transposeB, (j * ldb) + k, (k * ldb) + j));
                        auto cOffset = (i * ldc) + j;

                        // accumulate product in C[i, j]
                        C[cOffset] = C[cOffset] + (A[aOffset] * B[bOffset]);
                    });
                });
            });
            function.Return(function.Literal<int>(0));

            module.EndFunction();
            return function.GetFunction();
        }

    } // end anonymous namespace

    static const std::string& countName = "count";
    static const std::string& lVectorName = "pLVector";
    static const std::string& rVectorName = "pRVector";
    static const std::string& resultName = "pResult";

    static const std::string& dotProductFloatName = "DotProductFloat";
    static const std::string& dotProductIntName = "DotProductInt";
    static const std::string& getTimeFunctionName = "GetTime";

    IRRuntime::IRRuntime(IRModuleEmitter& module)
        : _module(module), _posixRuntime(module)
    {
    }

    LLVMType IRRuntime::GetIntType()
    {
        auto& context = _module.GetLLVMContext();
        const auto numBits = _module.GetCompilerOptions().targetDevice.numBits;
        if (numBits != 0)
        {
            return llvm::Type::getIntNTy(context, numBits);
        }
        else
        {
            return llvm::Type::getInt32Ty(context);
        }
    }

    std::string IRRuntime::GetNamespacePrefix() const
    {
        return _module.GetModuleName();
    }

    LLVMFunction IRRuntime::GetDotProductFloatFunction()
    {
        auto functionName = GetNamespacePrefix() + "_" + dotProductFloatName;
        NamedVariableTypeList argTypes = { { countName, VariableType::Int32 },
                                           { lVectorName, VariableType::DoublePointer },
                                           { rVectorName, VariableType::DoublePointer },
                                           { resultName, VariableType::DoublePointer } };
        auto function = _module.BeginFunction(functionName, VariableType::Void, argTypes);
        function.IncludeInHeader();

        auto arguments = function.Arguments().begin();
        llvm::Argument& count = *arguments++;
        llvm::Argument& leftValue = *arguments++;
        llvm::Argument& rightValue = *arguments++;
        llvm::Argument& result = *arguments++;
        function.DotProduct(&count, &leftValue, &rightValue, &result);
        function.Return();
        _module.EndFunction();

        return function.GetFunction();
    }

    LLVMFunction IRRuntime::GetDotProductIntFunction()
    {
        auto functionName = GetNamespacePrefix() + "_" + dotProductIntName;
        NamedVariableTypeList argTypes = { { countName, VariableType::Int32 },
                                           { lVectorName, VariableType::Int32Pointer },
                                           { rVectorName, VariableType::Int32Pointer },
                                           { resultName, VariableType::Int32Pointer } };
        auto function = _module.BeginFunction(functionName, VariableType::Void, argTypes);
        function.IncludeInHeader();

        auto arguments = function.Arguments().begin();
        llvm::Argument& count = *arguments++;
        llvm::Argument& leftValue = *arguments++;
        llvm::Argument& rightValue = *arguments++;
        llvm::Argument& result = *arguments++;
        function.DotProduct(&count, &leftValue, &rightValue, &result);
        function.Return();
        _module.EndFunction();
        return function.GetFunction();
    }

    LLVMFunction IRRuntime::ResolveCurrentTimeFunction(llvm::StructType* timespecType)
    {
        LLVMFunction function = nullptr;

        auto& context = _module.GetLLVMContext();
        auto int32Type = llvm::Type::getInt32Ty(context);
        auto int64Type = llvm::Type::getInt64Ty(context);
        auto doubleType = llvm::Type::getDoubleTy(context);

        auto tmFieldType = timespecType->getElementType(0); // The type of the first field of the timespec struct -- it's the correct bitsize for 'int'
        auto intType = tmFieldType;

        if (_module.GetCompilerOptions().targetDevice.IsWindows())
        {
            // We normally assume there is a system function "clock_gettime" that provides high resolution times for profiling the emitted model.
            // But this function doesn't exist on Windows.  So on Windows we implement this function to call the Win32 QueryPerformanceCounter API.
            // Like this:
            //
            //    int clock_gettime(int32_t clk_id, struct timespec *tp) {
            //        LARGE_INTEGER lp;
            //        LARGE_INTEGER freq;
            //        // this timer is in 100 nanosecond intervals.
            //        QueryPerformanceCounter(&lp);
            //        QueryPerformanceFrequency(&freq);
            //        double seconds = (double)lp.QuadPart / (double)freq.QuadPart;
            //        int32_t sec = (int32_t)seconds;
            //        tp->tv_nsec = (int32_t)((seconds - sec) * 10000000);
            //        tp->tv_sec = sec;
            //        return 0;
            //    }

            const VariableType tmFieldVarType = tmFieldType == int64Type ? VariableType::Int64 : VariableType::Int32;

            auto zero = llvm::ConstantInt::get(int32Type, 0);
            auto hundredNanoSeconds = llvm::ConstantFP::get(doubleType, 10000000.0);

            llvm::FunctionType* qpcProto = llvm::FunctionType::get(int32Type, { int64Type->getPointerTo() }, false);
            _module.DeclareFunction("QueryPerformanceCounter", qpcProto);
            _module.DeclareFunction("QueryPerformanceFrequency", qpcProto);
            auto qpcFunction = _module.GetFunction("QueryPerformanceCounter");
            auto qpfFunction = _module.GetFunction("QueryPerformanceFrequency");

            std::vector<LLVMType> args;
            args.push_back(int32Type);
            auto timeSpecPointerType = timespecType->getPointerTo();
            args.push_back(timeSpecPointerType);
            IRFunctionEmitter& emitter = _module.BeginFunction("clock_gettime", int32Type, args);
            function = emitter.GetFunction();

            // Get pointer to the timespec argument.
            auto iterator = function->arg_begin();
            iterator++; // get second argument
            auto timeSpecArg = &*iterator; // Get the timespec.

            // get access to the tv_sec and nano fields of the time struct argument.
            auto irBuilder = emitter.GetEmitter().GetIRBuilder();
            auto secondsPtr = irBuilder.CreateInBoundsGEP(timespecType, timeSpecArg, { emitter.Literal(0), emitter.Literal(0) });
            auto nanoPtr = irBuilder.CreateInBoundsGEP(timespecType, timeSpecArg, { emitter.Literal(0), emitter.Literal(1) });

            auto timeVar = emitter.Variable(int64Type, "time");
            auto freqVar = emitter.Variable(int64Type, "freq");

            // QueryPerformanceCounter(&lp);
            emitter.Call(qpcFunction, { timeVar });
            // QueryPerformanceFrequency(&freq);
            emitter.Call(qpfFunction, { freqVar });

            auto timeDouble = emitter.CastIntToFloat(emitter.Load(timeVar), VariableType::Double, true);
            auto freqDouble = emitter.CastIntToFloat(emitter.Load(freqVar), VariableType::Double, true);
            // double seconds = (double)time.QuadPart / (double)freq.QuadPart;
            auto seconds = emitter.Operator(TypedOperator::divideFloat, timeDouble, freqDouble);

            // int32_t sec = (int64_t)seconds;
            auto intSeconds = emitter.CastFloatToInt(seconds, VariableType::Int64);
            auto floatSeconds = emitter.CastIntToFloat(intSeconds, VariableType::Double, true);

            //  tp->tv_nsec = (int32_t)((seconds - sec) * 10000000);
            auto remainder = emitter.Operator(TypedOperator::subtractFloat, seconds, floatSeconds);
            auto nanoseconds = emitter.Operator(TypedOperator::multiplyFloat, remainder, hundredNanoSeconds);

            // STYLE matching casing style of timespec struct members
            auto tv_nsec = emitter.CastFloatToInt(nanoseconds, tmFieldVarType);
            emitter.Store(nanoPtr, tv_nsec);

            // tp->tv_sec = sec;
            // STYLE matching casing style of timespec struct members
            auto tv_sec = emitter.CastFloatToInt(floatSeconds, tmFieldVarType);
            emitter.Store(secondsPtr, tv_sec);

            emitter.Return(zero);
            _module.EndFunction();
        }
        else
        {
#ifndef WIN32
            // On non-Windows, we need to make sure the linker links in
            // the clock_gettime function.
            volatile void* temp = (void*)(&clock_gettime);
            UNUSED(temp);
#endif
            llvm::FunctionType* gettimeType = llvm::FunctionType::get(intType, { int32Type, timespecType->getPointerTo() }, false);
            _module.DeclareFunction("clock_gettime", gettimeType);
            function = _module.GetFunction("clock_gettime");
        }

        return function;
    }

    LLVMValue IRRuntime::GetCurrentTime(IRFunctionEmitter& function)
    {
        auto getTimeFunc = GetCurrentTimeFunction();
        auto time = function.Call(getTimeFunc, {});
        return time;
    }

    LLVMFunction IRRuntime::GetCurrentTimeFunction()
    {
        if (_pGetCurrentTimeFunction == nullptr)
        {
            auto& emitter = _module.GetIREmitter();
            auto& context = _module.GetLLVMContext();
            auto& irBuilder = emitter.GetIRBuilder();
            auto int32Type = llvm::Type::getInt32Ty(context);

            llvm::StructType* timespecType = _posixRuntime.GetTimespecType();
            llvm::FunctionType* gettimeType = llvm::FunctionType::get(int32Type, { int32Type, timespecType->getPointerTo() }, false);
            _module.DeclareFunction("clock_gettime", gettimeType);

            // make struct
            auto getTimeFunction = ResolveCurrentTimeFunction(timespecType);
            if (getTimeFunction != nullptr)
            {
                auto functionName = GetNamespacePrefix() + "_" + getTimeFunctionName;
                auto function = _module.BeginFunction(functionName, VariableType::Double);

                llvm::AllocaInst* timeStruct = function.Variable(timespecType, "tp");

#ifdef _MSC_VER
                int CLOCK_REALTIME = 0;
#endif

                auto callResult = function.Call(getTimeFunction, { function.Literal(CLOCK_REALTIME), timeStruct });
                UNUSED(callResult);

                // LLVMValue timeStructBase = irBuilder.CreateInBoundsGEP(timespecType, timeStruct, function.Literal(0));
                auto secondsPtr = irBuilder.CreateInBoundsGEP(timespecType, timeStruct, { function.Literal(0), function.Literal(0) });
                auto nanosecondsPtr = irBuilder.CreateInBoundsGEP(timespecType, timeStruct, { function.Literal(0), function.Literal(1) });
                auto secondsIntVal = function.Load(secondsPtr);
                auto nanosecondsIntVal = function.Load(nanosecondsPtr);
                auto secondsDoubleVal = emitter.CastIntToFloat(secondsIntVal, VariableType::Double, true);
                auto nanosecondsDoubleVal = emitter.CastIntToFloat(nanosecondsIntVal, VariableType::Double, false);
                auto divisor = function.Literal(1000000000.0);
                auto totalSecondsDoubleVal = function.Operator(TypedOperator::addFloat, secondsDoubleVal, function.Operator(TypedOperator::divideFloat, nanosecondsDoubleVal, divisor));
                function.Return(function.Operator(TypedOperator::multiplyFloat, totalSecondsDoubleVal, function.Literal(1000.0)));
                _module.EndFunction();
                _pGetCurrentTimeFunction = function.GetFunction();
            }
            else
            {
                throw EmitterException(EmitterError::functionNotFound);
            }
        }
        return _pGetCurrentTimeFunction;
    }

    LLVMFunction IRRuntime::GetSqrtFunction(VariableType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::sqrt, { argType });
    }

    LLVMFunction IRRuntime::GetAbsFunction(VariableType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::fabs, { argType });
    }

    LLVMFunction IRRuntime::GetExpFunction(VariableType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::exp, { argType });
    }

    LLVMFunction IRRuntime::GetLogFunction(VariableType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::log, { argType });
    }

    LLVMFunction IRRuntime::GetSinFunction(VariableType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::sin, { argType });
    }

    LLVMFunction IRRuntime::GetCosFunction(VariableType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::cos, { argType });
    }

    LLVMFunction IRRuntime::GetTanhFunction(VariableType argType)
    {
        // This assumes a standard C runtime library is linked
        switch (argType)
        {
        case VariableType::Float:
        case VariableType::Double:
            break;
        default:
            throw EmitterException(EmitterError::functionNotFound);
        }

        auto& emitter = _module.GetIREmitter();
        const char* funcName = argType == VariableType::Double ? "tanh" : "tanhf";
        auto valueType = emitter.Type(argType);
        auto tanhProto = llvm::FunctionType::get(valueType, { valueType }, false);
        _module.DeclareFunction(funcName, tanhProto);
        return _module.GetFunction(funcName);
    }

    LLVMFunction IRRuntime::GetSqrtFunction(LLVMType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::sqrt, { argType });
    }

    LLVMFunction IRRuntime::GetAbsFunction(LLVMType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::fabs, { argType });
    }

    LLVMFunction IRRuntime::GetExpFunction(LLVMType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::exp, { argType });
    }

    LLVMFunction IRRuntime::GetLogFunction(LLVMType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::log, { argType });
    }

    LLVMFunction IRRuntime::GetSinFunction(LLVMType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::sin, { argType });
    }

    LLVMFunction IRRuntime::GetCosFunction(LLVMType argType)
    {
        return _module.GetIntrinsic(llvm::Intrinsic::cos, { argType });
    }

    //
    // BLAS
    //
    LLVMFunction IRRuntime::GetSGEMVFunction(bool useBlas)
    {
        VariableTypeList argTypes = {
            VariableType::Int32, // order
            VariableType::Int32, // transpose
            VariableType::Int32, // m
            VariableType::Int32, // n
            VariableType::Float, // alpha
            VariableType::FloatPointer, // A
            VariableType::Int32, // lda
            VariableType::FloatPointer, // x
            VariableType::Int32, // incx
            VariableType::Float, // beta
            VariableType::FloatPointer, // y
            VariableType::Int32 // incy
        };

        auto pModule = _module.GetLLVMModule();
        if (useBlas)
        {
            auto types = _module.GetIREmitter().GetLLVMTypes(argTypes);
            auto functionType = llvm::FunctionType::get(_module.GetIREmitter().Type(emitters::VariableType::Int32), types, false);
            return static_cast<LLVMFunction>(pModule->getOrInsertFunction("cblas_sgemv", functionType));
        }
        else
        {
            auto pFunction = pModule->getFunction("noblas_sgemv");
            if (pFunction != nullptr)
            {
                return pFunction;
            }
            return EmitGEMVFunction<float>(_module, "noblas_sgemv", argTypes);
        }
    }

    LLVMFunction IRRuntime::GetDGEMVFunction(bool useBlas)
    {
        VariableTypeList argTypes = {
            VariableType::Int32, // order
            VariableType::Int32, // transpose
            VariableType::Int32, // m
            VariableType::Int32, // n
            VariableType::Double, // alpha
            VariableType::DoublePointer, // M
            VariableType::Int32, // lda
            VariableType::DoublePointer, // x
            VariableType::Int32, // incx
            VariableType::Double, // beta
            VariableType::DoublePointer, // y
            VariableType::Int32 // incy
        };

        auto pModule = _module.GetLLVMModule();
        if (useBlas)
        {
            auto types = _module.GetIREmitter().GetLLVMTypes(argTypes);
            auto functionType = llvm::FunctionType::get(_module.GetIREmitter().Type(emitters::VariableType::Int32), types, false);
            return static_cast<LLVMFunction>(pModule->getOrInsertFunction("cblas_dgemv", functionType));
        }
        else
        {
            auto pFunction = pModule->getFunction("noblas_dgemv");
            if (pFunction != nullptr)
            {
                return pFunction;
            }
            return EmitGEMVFunction<double>(_module, "noblas_dgemv", argTypes);
        }
    }

    LLVMFunction IRRuntime::GetSGEMMFunction(bool useBlas)
    {
        VariableTypeList argTypes = {
            VariableType::Int32, // order
            VariableType::Int32, // transposeA
            VariableType::Int32, // transposeB
            VariableType::Int32, // m
            VariableType::Int32, // n
            VariableType::Int32, // k
            VariableType::Float, // alpha
            VariableType::FloatPointer, // A
            VariableType::Int32, // lda
            VariableType::FloatPointer, // B
            VariableType::Int32, // ldb
            VariableType::Float, // beta
            VariableType::FloatPointer, // C
            VariableType::Int32 // ldc
        };

        auto pModule = _module.GetLLVMModule();
        if (useBlas)
        {
            auto types = _module.GetIREmitter().GetLLVMTypes(argTypes);
            auto functionType = llvm::FunctionType::get(_module.GetIREmitter().Type(emitters::VariableType::Int32), types, false);
            return static_cast<LLVMFunction>(pModule->getOrInsertFunction("cblas_sgemm", functionType));
        }
        else
        {
            auto pFunction = pModule->getFunction("noblas_sgemm");
            if (pFunction != nullptr)
            {
                return pFunction;
            }
            return EmitGEMMFunction<float>(_module, "noblas_sgemm", argTypes);
        }
    }

    LLVMFunction IRRuntime::GetDGEMMFunction(bool useBlas)
    {
        VariableTypeList argTypes = {
            VariableType::Int32, // order
            VariableType::Int32, // transposeA
            VariableType::Int32, // transposeB
            VariableType::Int32, // m
            VariableType::Int32, // n
            VariableType::Int32, // k
            VariableType::Double, // alpha
            VariableType::DoublePointer, // A
            VariableType::Int32, // lda
            VariableType::DoublePointer, // B
            VariableType::Int32, // ldb
            VariableType::Double, // beta
            VariableType::DoublePointer, // C
            VariableType::Int32 // ldc
        };

        auto pModule = _module.GetLLVMModule();
        if (useBlas)
        {
            auto types = _module.GetIREmitter().GetLLVMTypes(argTypes);
            auto functionType = llvm::FunctionType::get(_module.GetIREmitter().Type(emitters::VariableType::Int32), types, false);
            return static_cast<LLVMFunction>(pModule->getOrInsertFunction("cblas_dgemm", functionType));
        }
        else
        {
            auto pFunction = pModule->getFunction("noblas_dgemm");
            if (pFunction != nullptr)
            {
                return pFunction;
            }
            return EmitGEMMFunction<double>(_module, "noblas_dgemm", argTypes);
        }
    }

    template <>
    LLVMFunction IRRuntime::GetGEMVFunction<float>(bool useBlas)
    {
        return GetSGEMVFunction(useBlas);
    }

    template <>
    LLVMFunction IRRuntime::GetGEMVFunction<double>(bool useBlas)
    {
        return GetDGEMVFunction(useBlas);
    }

    template <>
    LLVMFunction IRRuntime::GetGEMMFunction<float>(bool useBlas)
    {
        return GetSGEMMFunction(useBlas);
    }

    template <>
    LLVMFunction IRRuntime::GetGEMMFunction<double>(bool useBlas)
    {
        return GetDGEMMFunction(useBlas);
    }

    LLVMFunction IRRuntime::GetOpenBLASGetNumThreadsFunction()
    {
        // int openblas_get_num_threads();
        auto pModule = _module.GetLLVMModule();
        llvm::FunctionType* functionType = llvm::FunctionType::get(GetIntType(), {}, false);
        return static_cast<LLVMFunction>(pModule->getOrInsertFunction("openblas_get_num_threads", functionType));
    }

    LLVMFunction IRRuntime::GetOpenBLASSetNumThreadsFunction()
    {
        // void openblas_set_num_threads(int num_threads);
        auto pModule = _module.GetLLVMModule();
        auto& context = _module.GetLLVMContext();
        auto voidType = llvm::Type::getVoidTy(context);

        llvm::FunctionType* functionType = llvm::FunctionType::get(voidType, { GetIntType() }, false);
        return static_cast<LLVMFunction>(pModule->getOrInsertFunction("openblas_set_num_threads", functionType));
    }
}
}

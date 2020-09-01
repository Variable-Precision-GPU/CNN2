﻿#include "ffCudaNn.h"

#include <cuda_runtime.h>
#include <random>
#include <assert.h>

namespace ff
{
	///////////////////////////////////////////////////////////////////////
	static std::default_random_engine g_generator;
	static std::normal_distribution<double> g_distribution;

	CudaTensor::CudaTensor() : _d0(0), _d1(0), _d2(0), _d3(0), _dataSize(0), _dataGpu(nullptr), _dataGpuSize(0)
	{
	}

	CudaTensor::CudaTensor(int d0, int d1, int d2, int d3) : _dataGpu(nullptr), _dataGpuSize(0)
	{
		ResetTensor(d0, d1, d2, d3);
	}

	CudaTensor::~CudaTensor()
	{
		if (nullptr != _dataGpu) cudaFree(_dataGpu);
	}

	void CudaTensor::ResetTensor(int d0, int d1, int d2, int d3)
	{
		_d0 = d0; _d1 = d1; _d2 = d2; _d3 = d3;
		_dataSize = _d0 * _d1 * _d2 * _d3;
		_data.resize(_dataSize);

		if (_dataGpuSize < _dataSize)
		{
			_dataGpuSize = _dataSize;
			if (_dataGpu) cudaFree(_dataGpu);
			cudaError_t err = cudaMalloc(&_dataGpu, _dataGpuSize * sizeof(double));
			assert(err == cudaSuccess);
		}
	}

	void CudaTensor::Random(const double multiplier)
	{
		for (int i = 0; i < _dataSize; ++i)
		{
			_data[i] = g_distribution(g_generator) * multiplier;
		}
		cudaError_t err = cudaMemcpy(_dataGpu, &_data[0], _dataSize * sizeof(double), cudaMemcpyKind::cudaMemcpyHostToDevice);
		assert(err == cudaSuccess);
	}

	void CudaTensor::Zero()
	{
		memset(&_data[0], 0, _data.size() * sizeof(double));
		cudaError_t err = cudaMemcpy(_dataGpu, &_data[0], _dataSize * sizeof(double), cudaMemcpyKind::cudaMemcpyHostToDevice);
		assert(err == cudaSuccess);
	}

	void CudaTensor::Push()
	{
		cudaError_t err = cudaMemcpy(_dataGpu, &_data[0], _dataSize * sizeof(double), cudaMemcpyKind::cudaMemcpyHostToDevice);
		assert(err == cudaSuccess);
	}

	void CudaTensor::Pull()
	{
		cudaError_t err = cudaMemcpy(&_data[0], _dataGpu, _dataSize * sizeof(double), cudaMemcpyKind::cudaMemcpyDeviceToHost);
		assert(err == cudaSuccess);
	}

	///////////////////////////////////////////////////////////////////////
	__global__ void LinearTransform_Cuda(double* y, const double* x, const double* w, const double* b, int xw, int ww)
	{
		int r = blockIdx.x;
		int c = threadIdx.x;
		double v = 0.0;
		for (int i = 0; i < xw; ++i)
		{
			v += x[i + r * xw] * w[c + i * ww];
		}
		y[c + r * ww] = v + b[c];
	}

	__global__ void ComputeWg_Cuda(double* wG, const double* x, const double* yG, int nXcol, int nXrow, int nWcol)
	{
		// wG = x.T * yG
		int r = blockIdx.x;
		int c = threadIdx.x;
		double v = 0.0;
		for (int i = 0; i < nXrow; ++i)
		{
			v += x[r + i * nXcol] * yG[c + i * nWcol];
		}
		wG[c + r * nWcol] = v;
	}

	__global__ void ComputeXg_Cuda(double* xG, const double* yG, const double* w, int yGw, int wTh, int xGw)
	{
		// assert(yGw == wTh);
		// xG = yG * w.T
		int r = blockIdx.x;
		int c = threadIdx.x;
		double v = 0.0;
		for (int i = 0; i < yGw; ++i)
		{
			v += yG[i + r * yGw] * w[i + c * wTh];
		}
		xG[c + r * xGw] = v;
	}

	__global__ void ComputeBg_Cuda(double* bG, const double* yG, int nYgRow)
	{
		int c = threadIdx.x;
		bG[c] = 0.0;
		for (int i = 0; i < nYgRow; ++i)
		{
			bG[c] += yG[c + i * nYgRow];
		}
	}

	__global__ void ComputeSumOfSquresGradient(double* yG, const double* y, const double* yLabel, int nCol)
	{
		int r = blockIdx.x;
		int c = threadIdx.x;
		int index = c + r * nCol;
		double diff = y[index] - yLabel[index];
		yG[index] = 2.0f * diff;
	}

	__global__ void UpdateWs_Cuda(int nCol, double learningRate, double beta1, double beta2, double beta1t, double beta2t,
		double* w, const double* wG, double* wG_m, double* wG_v)
	{
		int r = blockIdx.x;
		int c = threadIdx.x;
		int index = c + r * nCol;

		// Vanilla
		//w[index] -= wG[index] * learningRate;

		// Adam
		wG_m[index] = beta1 * wG_m[index] + (1.0 - beta1) * wG[index];
		wG_v[index] = beta2 * wG_v[index] + (1.0 - beta2) * wG[index] * wG[index];
		double unbiased_m = wG_m[index] / (1.0 - beta1t);
		double unbiased_v = wG_v[index] / (1.0 - beta2t);
		w[index] -= (learningRate * unbiased_m / (sqrt(unbiased_v) + 1e-8));
	}

	__global__ void UpdateBs_Cuda(double learningRate, double beta1, double beta2, double beta1t, double beta2t,
		double* b, const double* bG, double* bG_m, double* bG_v)
	{
		int index = threadIdx.x;

		// Vanilla
		//b[index] -= bG[index] * learningRate;

		// Adam
		bG_m[index] = beta1 * bG_m[index] + (1.0 - beta1) * bG[index];
		bG_v[index] = beta2 * bG_v[index] + (1.0 - beta2) * bG[index] * bG[index];
		double unbiased_m = bG_m[index] / (1.0 - beta1t);
		double unbiased_v = bG_v[index] / (1.0 - beta2t);
		b[index] -= (learningRate * unbiased_m / (sqrt(unbiased_v) + 1e-8));
	}

	__global__ void Relu_Cuda(double* relu_x, const double* x, int nCol)
	{
		int r = blockIdx.x;
		int c = threadIdx.x;
		int index = c + r * nCol;
		relu_x[index] = fmax(x[index], 0.0);
	}

	__global__ void ReluG_Cuda(double* xG, const double* x, int nCol)
	{
		int r = blockIdx.x;
		int c = threadIdx.x;
		int index = c + r * nCol;
		xG[index] = xG[index] * (fmax(x[index], 1e-32) / x[index]);
		//if (x[index] < 0.0) xG[index] = 0.0;
	}

	__global__ void ForwardSoftmax_Step1_Cuda(double* sum, const double* x, int nRow, int nCol)
	{
		const int kThreadPerBlock = 64; // Note(dongwook): Should be the same value in SoftmaxLayer::Forward()
		int r = blockIdx.x * kThreadPerBlock + threadIdx.x;
		if (nRow <= r) return;
		sum[0 + r * nCol] = 1e-8;
		for (int i = 0; i < nCol; ++i)
		{
			sum[0 + r * nCol] += exp(x[i + r * nCol]);
		}
	}

	__global__ void ForwardSoftmax_Step2_Cuda(double* softmax, const double* sum, const double* x, int nRow, int nCol)
	{
		const int kThreadPerBlock = 64; // Note(dongwook): Should be the same value in SoftmaxLayer::Forward()
		int r = blockIdx.x * kThreadPerBlock + threadIdx.x;
		if (nRow <= r) return;
		for (int i = 0; i < nCol; ++i)
		{
			softmax[i + r * nCol] = exp(x[i + r * nCol]) / sum[0 + r * nCol];
		}
	}

	__global__ void SoftmaxBackward_Cuda(double* lossG, const double* softmax, const double* yLabel, int nCol)
	{
		int r = blockIdx.x;
		int c = threadIdx.x;
		int index = c + r * nCol;
		lossG[index] = softmax[index];
		if ((int)yLabel[r] == c) lossG[index] -= 1.0;
	}

	///////////////////////////////////////////////////////////////////////
	FcLayer::FcLayer(int inDim, int outDim) : _pX(nullptr)
	{
		_w.ResetTensor(outDim, inDim);
		_w.Random(1.0 / sqrt(inDim));
		_wG.ResetTensor(outDim, inDim);
		_wG_m.ResetTensor(outDim, inDim);
		_wG_m.Zero();
		_wG_v.ResetTensor(outDim, inDim);
		_wG_v.Zero();
		_b.ResetTensor(outDim);
		_b.Zero();
		_bG.ResetTensor(outDim);
		_bG_m.ResetTensor(outDim);
		_bG_m.Zero();
		_bG_v.ResetTensor(outDim);
		_bG_v.Zero();
	}

	const CudaTensor* FcLayer::Forward(const CudaTensor* x)
	{
		assert(x->_d0 == _w._d1);

		_pX = x;
		_y.ResetTensor(_w._d0, _pX->_d1);

		// y = xw+b
		dim3 block(x->_d1), threads(_w._d0);
		LinearTransform_Cuda << < block, threads >> > (_y._dataGpu, _pX->_dataGpu, _w._dataGpu, _b._dataGpu, _pX->_d0, _w._d0);
		assert(cudaGetLastError() == cudaSuccess);
		return &_y;
	}

	const CudaTensor* FcLayer::Backward(const CudaTensor* yG, const int layerIndex)
	{
		assert(yG->_d0 == _wG._d0);
		{
			dim3 block(_wG._d1), threads(_wG._d0);
			ComputeWg_Cuda << < block, threads >> > (_wG._dataGpu, _pX->_dataGpu, yG->_dataGpu, _pX->_d0, _pX->_d1, _wG._d0);
			assert(cudaGetLastError() == cudaSuccess);
		}

		{
			dim3 block(1), threads(_b._d0);
			ComputeBg_Cuda << < block, threads >> > (_bG._dataGpu, yG->_dataGpu, yG->_d1);
			assert(cudaGetLastError() == cudaSuccess);
		}

		if (layerIndex > 0)
		{
			assert(yG->_d1 == _pX->_d1);
			_xG.ResetTensor(_pX->_d0, _pX->_d1);
			dim3 block(_xG._d1), threads(_xG._d0);
			ComputeXg_Cuda << < block, threads >> > (_xG._dataGpu, yG->_dataGpu, _w._dataGpu, yG->_d0, _w._d0, _xG._d0);
			assert(cudaGetLastError() == cudaSuccess);
		}
		return &_xG;
	}

	void FcLayer::UpdateWs(double learningRate, double beta1, double beta2, double beta1t, double beta2t)
	{
		{
			dim3 block(_w._d1), threads(_w._d0);
			UpdateWs_Cuda << <block, threads >> > (_w._d0, learningRate, beta1, beta2, beta1t, beta2t, _w._dataGpu, _wG._dataGpu, _wG_m._dataGpu, _wG_v._dataGpu);
			assert(cudaGetLastError() == cudaSuccess);
		}
		{
			dim3 block(1), threads(_b._d0);
			UpdateBs_Cuda << < block, threads >> > (learningRate, beta1, beta2, beta1t, beta2t, _b._dataGpu, _bG._dataGpu, _bG_m._dataGpu, _bG_v._dataGpu);
			assert(cudaGetLastError() == cudaSuccess);
		}
	}

	ReluFcLayer::ReluFcLayer(int inDim, int outDim) : FcLayer(inDim, outDim)
	{
	}

	const CudaTensor* ReluFcLayer::Forward(const CudaTensor* x)
	{
		assert(x->_d0 == _w._d1);

		_pX = x;
		_xRelu.ResetTensor(_pX->_d0, _pX->_d1);
		{
			dim3 block(_xRelu._d1), threads(_xRelu._d0);
			Relu_Cuda << < block, threads >> > (_xRelu._dataGpu, _pX->_dataGpu, _xRelu._d0);
			assert(cudaGetLastError() == cudaSuccess);
		}

		_y.ResetTensor(_w._d0, _xRelu._d1);
		{
			// y = xw+b
			dim3 block(_xRelu._d1), threads(_w._d0);
			LinearTransform_Cuda << < block, threads >> > (_y._dataGpu, _xRelu._dataGpu, _w._dataGpu, _b._dataGpu, _xRelu._d0, _w._d0);
			assert(cudaGetLastError() == cudaSuccess);
		}

		return &_y;
	}

	const CudaTensor* ReluFcLayer::Backward(const CudaTensor* yG, const int layerIndex)
	{
		assert(yG->_d0 == _wG._d0);
		{
			dim3 block(_wG._d1), threads(_wG._d0);
			ComputeWg_Cuda << < block, threads >> > (_wG._dataGpu, _xRelu._dataGpu, yG->_dataGpu, _xRelu._d0, _xRelu._d1, _wG._d0);
			assert(cudaGetLastError() == cudaSuccess);
		}

		{
			dim3 block(1), threads(_b._d0);
			ComputeBg_Cuda << < block, threads >> > (_bG._dataGpu, yG->_dataGpu, yG->_d1);
			assert(cudaGetLastError() == cudaSuccess);
		}

		if (layerIndex > 0)
		{
			{
				assert(yG->_d1 == _pX->_d1);
				_xG.ResetTensor(_pX->_d0, _pX->_d1);
				dim3 block(_xG._d1), threads(_xG._d0);
				ComputeXg_Cuda << < block, threads >> > (_xG._dataGpu, yG->_dataGpu, _w._dataGpu, yG->_d0, _w._d0, _xG._d0);
				assert(cudaGetLastError() == cudaSuccess);
			}
			{
				dim3 block(_xG._d1), threads(_xG._d0);
				ReluG_Cuda << < block, threads >> > (_xG._dataGpu, _pX->_dataGpu, _xG._d0);
				assert(cudaGetLastError() == cudaSuccess);
			}
		}

		return &_xG;
	}

	const CudaTensor* SoftmaxLayer::Forward(const CudaTensor* x)
	{
		_softmax.ResetTensor(x->_d0, x->_d1);
		_lossG.ResetTensor(x->_d0, x->_d1);

		const int kThreadPerBlock = 64; // Note(dongwook): Should be the same value in ForwardSoftmax_*_Cuda()
		int nBlocks = (x->_d1 + kThreadPerBlock - 1) / kThreadPerBlock;
		dim3 block(nBlocks), threads(kThreadPerBlock);
		ForwardSoftmax_Step1_Cuda << < block, threads >> > (_lossG._dataGpu, x->_dataGpu, x->_d1, x->_d0);
		assert(cudaGetLastError() == cudaSuccess);
		ForwardSoftmax_Step2_Cuda << < block, threads >> > (_softmax._dataGpu, _lossG._dataGpu, x->_dataGpu, x->_d1, x->_d0);
		assert(cudaGetLastError() == cudaSuccess);
		return &_softmax;
	}

	const CudaTensor* SoftmaxLayer::Backward(const CudaTensor* yG, const int layerIndex)
	{
		assert(yG->_d0 == _lossG._d1);
		dim3 block(_lossG._d1), threads(_lossG._d0);
		SoftmaxBackward_Cuda << < block, threads >> > (_lossG._dataGpu, _softmax._dataGpu, yG->_dataGpu, _lossG._d0);
		assert(cudaGetLastError() == cudaSuccess);
		return &_lossG;
	}

	SumOfSquaresLayer::SumOfSquaresLayer() : _pY(nullptr)
	{
	}

	const CudaTensor* SumOfSquaresLayer::Forward(const CudaTensor* x)
	{
		_pY = x;
		return _pY;
	}

	const CudaTensor* SumOfSquaresLayer::Backward(const CudaTensor* yLabel, const int layerIndex)
	{
		_yG.ResetTensor(yLabel->_d0, yLabel->_d1);

		dim3 block(_yG._d1), threads(_yG._d0);
		ComputeSumOfSquresGradient << < block, threads >> > (_yG._dataGpu, _pY->_dataGpu, yLabel->_dataGpu, _yG._d0);
		assert(cudaGetLastError() == cudaSuccess);
		return &_yG;
	}

	///////////////////////////////////////////////////////////////////////
	CudaNn::CudaNn() : _beta1t(kBeta1), _beta2t(kBeta2)
	{
	}

	CudaNn::~CudaNn()
	{
		InitializeCudaNn("");
	}

	bool CudaNn::InitializeCudaNn(const char* desc)
	{
		size_t numLayers = _layers.size();
		for (size_t i = 0; i < numLayers; ++i)
		{
			delete _layers[i];
		}
		_layers.clear();

		return true;
	}

	bool CudaNn::AddFc(int inDim, int outDim)
	{
		_layers.push_back(new FcLayer(inDim, outDim));
		return true;
	}

	bool CudaNn::AddReluFc(int inDim, int outDim)
	{
		_layers.push_back(new ReluFcLayer(inDim, outDim));
		return true;
	}

	bool CudaNn::AddSoftmax()
	{
		_layers.push_back(new SoftmaxLayer);
		return true;
	}

	bool CudaNn::AddSumOfSquares()
	{
		_layers.push_back(new SumOfSquaresLayer);
		return true;
	}

	const CudaTensor* CudaNn::Forward(const CudaTensor* x)
	{
		const CudaTensor* y = nullptr;
		size_t numLayer = _layers.size();
		for (size_t i = 0; i < numLayer; ++i)
		{
			if (nullptr == x)
				return nullptr;

			y = _layers[i]->Forward(x);
			x = y;
		}
		return y;
	}

	void CudaNn::Backward(const CudaTensor* yLabel)
	{
		const CudaTensor* y = yLabel;
		const CudaTensor* yGradient = nullptr;
		int numLayer = (int)_layers.size();
		for (int i = 0; i < numLayer; ++i)
		{
			int layerIndex = numLayer - i - 1;
			yGradient = _layers[layerIndex]->Backward(y, layerIndex);
			y = yGradient;
		}
	}

	void CudaNn::UpdateWs(double learningRate)
	{
		int numLayer = (int)_layers.size();
		for (int i = 0; i < numLayer; ++i)
		{
			_layers[i]->UpdateWs(learningRate, kBeta1, kBeta2, _beta1t, _beta2t);
		}
		_beta1t *= kBeta1;
		_beta2t *= kBeta2;
	}
} // namespace ff
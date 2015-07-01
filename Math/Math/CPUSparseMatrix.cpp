//
// <copyright file="CPUSparseMatrix.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// Math.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include <assert.h>
#include <stdexcept>
#include <omp.h>
#include <math.h>
#include "CPUMatrix.h"
#include "CPUSparseMatrix.h"
#include <random>
#include <chrono>
#ifdef    _WIN32
#include <Windows.h>
#endif
#ifdef LEAKDETECT
#include <vld.h>
#endif

#include "Basics.h"
#include "fileutil.h"
#include <iostream>
#pragma warning (disable: 4127) // conditional expression is constant; "if (sizeof(ElemType)==sizeof(float))" triggers this

#ifndef USE_MKL
// use ACML as default. 
// Download ACML 5.3.0 (e.g., acml5.3.0-ifort64.exe) or above 
// from http://developer.amd.com/tools/cpu-development/amd-core-math-library-acml/acml-downloads-resources/
// Install the ifort64 variant (compiled with intel compiler) of the library
// Set Environment variable ACML_PATH to C:\AMD\acml5.3.0\ifort64_mp or the folder you installed acml
// to point to your folder for the include file and link library
#include <acml.h>  // requires ACML 5.3.0 and above
#else
// requires MKL 10.0 and above
#include <mkl.h>
#endif

// This is an example of an exported variable
//MATH_API int nMath=0;

// This is an example of an exported function.
//MATH_API int fnMath(void)
//{
//    return 42;
//}

#ifndef USE_MKL  //MKL has one additional parameter for different matrix order
#define BLAS_COLMAJOR 
#else
#define BLAS_COLMAJOR (int)MatrixOrder::ColMajor, 
#endif

#define SWAP(a,b) {(a) ^= (b); (b) ^= (a); (a) ^= (b);}
#define IDX2C(i,j,ld) (((j)*(ld))+(i)) // 0 based indexing
namespace Microsoft { namespace MSR { namespace CNTK {

#pragma region Helpful Enum Definitions
    enum class MatrixOrder
    {
        RowMajor = 101,  // row-major arrays 
        ColMajor = 102  // column-major arrays 
    };

    enum class MatrixTranspose : char
    {
        NoTrans = 'N', // trans='N'
        Trans = 'T', // trans='T' 
        ConjTrans = 'C' // trans='C'
    };

    enum class SymMatrixType : char
    {
        Up = 'U', // symmetric matrix is stored in the upper part
        Low = 'L', // symmetric matrix is stored in thelower part
        Full = 'F', //full populated
        NotSymmetric = 'N' //not a symmetric matrix
    };

    enum class MatrixOpSide : char
    {
        Left = 'L', // left multiply
        Right = 'R', // right multiply
    };
#pragma endregion Helpful Enum Definitions

#pragma region Constructors and Destructor

    //should only be used by constructors.
    template<class ElemType>
    void CPUSparseMatrix<ElemType>::ZeroInit()
    {   
        m_numRows = 0;
        m_numCols = 0;
        m_elemSizeAllocated = 0;
        m_compIndexSize = 0;
        m_externalBuffer = false;
        m_computeDevice = CPUDEVICE;
        m_nz = 0;
        m_matrixName = NULL;   

        //if(m_format == MatrixFormat::matrixFormatSparseCSC || m_format == MatrixFormat::matrixFormatSparseCSR) 
        {
            m_colIdx = -1;
            m_pArray = NULL;
            m_unCompIndex = NULL;
            m_compIndex = NULL;
        } 
        //else if (m_format == MatrixFormat::matrixFormatSparseBlockCol || m_format == MatrixFormat::matrixFormatSparseBlockRow) 
        {
            m_blockSize = 0;      
            m_pArray = NULL;
            m_blockIds = NULL;
        }
    }

    //should only be used by constructors.
    template<class ElemType>
    void CPUSparseMatrix<ElemType>::CheckInit(const MatrixFormat format)
    {
        if (format != MatrixFormat::matrixFormatSparseCSC && format != MatrixFormat::matrixFormatSparseCSR && format != MatrixFormat::matrixFormatSparseBlockCol && format != MatrixFormat::matrixFormatSparseBlockRow)
        {
            throw std::logic_error("CPUSparseMatrix:  unsupported sparse matrix format");
        }
        m_format = format;
        ZeroInit();
    }

    template<class ElemType>
    CPUSparseMatrix<ElemType>::CPUSparseMatrix(const MatrixFormat format)
    {

        CheckInit(format);
    }

    template<class ElemType>
    CPUSparseMatrix<ElemType>::CPUSparseMatrix(const MatrixFormat format, const size_t numRows, const size_t numCols, const size_t size)
    {

        CheckInit(format);
        Resize(numRows, numCols, size, true, false);
    }

    template<class ElemType>
    CPUSparseMatrix<ElemType>::~CPUSparseMatrix()
    {               

        if (m_matrixName!=NULL) 
        {
            delete[] m_matrixName;
            m_matrixName = nullptr;
        }
        if(m_format == MatrixFormat::matrixFormatSparseCSC || m_format == MatrixFormat::matrixFormatSparseCSR) 
        {
            if(m_pArray != NULL) 
                delete[] m_pArray;
            if(m_unCompIndex != NULL) 
                delete[] m_unCompIndex;
            if(m_compIndex != NULL)
                delete[] m_compIndex;
        }  
        else if (m_format == MatrixFormat::matrixFormatSparseBlockCol || m_format == MatrixFormat::matrixFormatSparseBlockRow) 
        {
            if (m_pArray != NULL)
                delete[] m_pArray;
            if(m_blockIds != NULL) 
                delete[] m_blockIds;
        }
    }



#pragma endregion Constructors and Destructor

#pragma region Basic Operators

    //make sure call order in colume wise for CSC and row wise for CSR
    template<class ElemType>
    void CPUSparseMatrix<ElemType>::SetValue(const size_t row, const size_t col, const ElemType v)
    {
        if(m_format != MatrixFormat::matrixFormatSparseCSC && m_format != MatrixFormat::matrixFormatSparseCSR) 
        {
            throw std::logic_error("CPUSparseMatrix:  unsupported SetValue() call.");
        }

        if(m_elemSizeAllocated < m_nz +1) //automatic resize
        {
            Resize(m_numRows, m_numCols, m_nz + 100, true, true);  //allocate 100 more elelemnts and keep existing values
        }

        if(row < 0 || row >= m_numRows) 
        {
            throw std::logic_error("CPUSparseMatrix: SetValue() invalid row id");
        }

        if(col < 0 || col >= m_numCols) {
            throw std::logic_error("CPUSparseMatrix: SetValue() invalid column id");
        }

        size_t r = (m_format == matrixFormatSparseCSC) ? row: col;
        size_t c = (m_format == matrixFormatSparseCSC) ? col: row;

        m_pArray[m_nz] = v;
        m_unCompIndex[m_nz] = (CPUSPARSE_INDEX_TYPE)r;

        //consistency check
        if (m_nz > 0)
        {
            if(c == m_colIdx && r <= m_unCompIndex[m_nz-1]) 
            {
                throw std::logic_error("CPUSparseMatrix:  SetValue is not called properly");
            }
        }

        if (c != m_colIdx) 
        {
            m_compIndex[c] = CPUSPARSE_INDEX_TYPE(m_nz);
            m_colIdx = (int) c;
        } 
        m_compIndex[c + 1] = CPUSPARSE_INDEX_TYPE(m_nz + 1);
        m_nz++;
    }

	template<class ElemType>
	void CPUSparseMatrix<ElemType>::Print(const char* matrixName) const {
		Print(matrixName, 0, 0, 0, 0);
	}

	template<class ElemType>
	void CPUSparseMatrix<ElemType>::Print(const char* matrixName, size_t /*rowStart*/, size_t /*rowEnd*/, size_t /*colStart*/, size_t /*colEnd*/) const {

		if (this->GetFormat() != matrixFormatSparseCSC && this->GetFormat() != matrixFormatSparseCSR)
		{
			return;
			//NOT_IMPLEMENTED;
		}

		fprintf(stderr, "%s\n", matrixName);

		const ElemType* dataBuffer = NzValues();
		const size_t nz = MajorIndexCount();
		CPUSPARSE_INDEX_TYPE* unCompressedIndex = MajorIndexLocation();
		CPUSPARSE_INDEX_TYPE* compressedIndex = SecondaryIndexLocation();

		for (size_t i = 0, j = 0; i < nz; ++i)
		{
			if (i >= compressedIndex[j]){
				fprintf(stderr, "\n");
				j++;
			}
			fprintf(stderr, "%d:%.f ", unCompressedIndex[i], dataBuffer[i]);
		}
		fprintf(stderr, "\n");
    }

    template<class ElemType>
    CPUMatrix<ElemType> CPUSparseMatrix<ElemType>::ColumnSliceToDense(size_t startColumn, size_t numCols) const
    {
        if (numCols == 0)
            throw std::logic_error("The slice cannot have 0 columns.");

        if (startColumn + numCols > m_numCols)
            throw std::logic_error("The slice is out of range of the source matrix.");

        if (m_format != MatrixFormat::matrixFormatSparseCSC)
            NOT_IMPLEMENTED;

        CPUMatrix<ElemType> slice(m_numRows, numCols);

#pragma omp parallel for
        for (long j = 0; j < numCols; j++)
        {
            long start = (long)m_compIndex[startColumn + j];
            long end = (long)m_compIndex[startColumn + j + 1];

            for (long p = start; p < end; p++)
            {
                size_t i = m_unCompIndex[p];
                ElemType value = m_pArray[(size_t)p];
                slice(i, (size_t)j) = value;
            }
        }

        return slice;
    }

    template<class ElemType>
    void CPUSparseMatrix<ElemType>::SetMatrixFromCSCFormat(const CPUSPARSE_INDEX_TYPE *h_CSCCol, const CPUSPARSE_INDEX_TYPE *h_Row, const ElemType *h_Val,
        const size_t nz, const size_t numRows, const size_t numCols)
    {
        m_format = matrixFormatSparseCSC;
        Resize(numRows, numCols, nz, true, false);
        this->SetNzCount(nz);

        memcpy(RowLocation(), h_Row, RowSize());
        memcpy(ColLocation(), h_CSCCol, ColSize());
        memcpy(NzValues(), h_Val, NzSize());
    }

    template<class ElemType>
    ElemType* CPUSparseMatrix<ElemType>::BufferPointer() const
    {
        return m_pArray;
    }

    template<class ElemType>
    void CPUSparseMatrix<ElemType>::Resize(const size_t numRows, const size_t numCols, size_t numNZElemToReserve, const bool growOnly, bool keepExistingValues)
    {               
        if (m_numRows != numRows || m_numCols != numCols)
            keepExistingValues = false;  

        size_t newCompIndexSize = (numCols > numRows ? numCols : numRows) + 1;
        bool reallocate = (m_elemSizeAllocated < numNZElemToReserve || (m_elemSizeAllocated > numNZElemToReserve && !growOnly) || m_compIndexSize < newCompIndexSize);

        m_numRows = numRows;
        m_numCols = numCols;

        if (reallocate)
        {                
            if (m_format == MatrixFormat::matrixFormatSparseCSC || m_format == MatrixFormat::matrixFormatSparseCSR)
            {
                ElemType *pArray = new ElemType[numNZElemToReserve];
                CPUSPARSE_INDEX_TYPE *unCompIndex = new CPUSPARSE_INDEX_TYPE[numNZElemToReserve];
                CPUSPARSE_INDEX_TYPE *compIndex = new CPUSPARSE_INDEX_TYPE[newCompIndexSize];
                
                if (keepExistingValues && (m_nz > numNZElemToReserve || m_compIndexSize > newCompIndexSize))
                    LogicError("Resize: To keep values m_nz should <= numNZElemToReserve and m_compIndexSize <= newCompIndexSize");

                if (keepExistingValues && m_nz > 0)
                {
                    assert(m_compIndexSize > 0 && m_nz < numNZElemToReserve);
                    memcpy(pArray, m_pArray, NzSize());
                    memcpy(unCompIndex, m_unCompIndex, MajorIndexSize());
                    memcpy(compIndex, m_compIndex, SecondaryIndexSize());
                }

                if (m_pArray != NULL)
                    delete[] m_pArray;
                if (m_unCompIndex != NULL)
                    delete[] m_unCompIndex;
                if (m_compIndex != NULL)
                    delete[] m_compIndex;

                m_pArray = pArray;
                m_unCompIndex = unCompIndex;
                m_compIndex = compIndex;
            }
            else if(m_format == MatrixFormat::matrixFormatSparseBlockCol || m_format == MatrixFormat::matrixFormatSparseBlockRow) 
            {
                ElemType *blockVal = new ElemType[numNZElemToReserve];
                size_t *blockIds = new size_t[newCompIndexSize];

                if (keepExistingValues && (m_nz > numNZElemToReserve || m_compIndexSize > newCompIndexSize))
                    throw std::logic_error("Resize: To keep values m_nz should <= numNZElemToReserve and m_compIndexSize <= newCompIndexSize");

                if (keepExistingValues && m_elemSizeAllocated > 0)
                {
                    assert(m_compIndexSize > 0 && m_elemSizeAllocated < numNZElemToReserve);
                    memcpy(blockVal, m_pArray, NzSize());
                    memcpy(blockIds, m_blockIds, sizeof(size_t)*m_compIndexSize);
                }

                if (m_pArray != NULL)
                    delete[] m_pArray;
                if(m_blockIds != NULL) 
                    delete[] m_blockIds;

                m_pArray = blockVal;
                m_blockIds = blockIds;
            }

            m_elemSizeAllocated = numNZElemToReserve;
            m_compIndexSize = newCompIndexSize;
        }
    }

    //Reset matrix so it can be reused
    template<class ElemType>
    void CPUSparseMatrix<ElemType>::Reset()
    {                
        m_nz = 0;
        m_colIdx = -1;
        m_blockSize = 0;
    }

    //c = alpha*op(lhs) * op(rhs) + beta*c
    template<class ElemType>
    void CPUSparseMatrix<ElemType>::MultiplyAndWeightedAdd(ElemType alpha, const CPUMatrix<ElemType>& lhs, const bool transposeA, 
        const CPUSparseMatrix<ElemType>& rhs, const bool transposeB, ElemType beta, CPUMatrix<ElemType>& c)

    {
        if (lhs.IsEmpty() || rhs.IsEmpty())
            throw std::logic_error("MultiplyAndWeightedAdd:  one of the input matrix is empty.");

        int m = transposeA? (int)lhs.GetNumCols(): (int)lhs.GetNumRows();
        int k = transposeA? (int)lhs.GetNumRows(): (int)lhs.GetNumCols();
        int l = transposeB? (int)rhs.GetNumCols(): (int)rhs.GetNumRows();
        int n = transposeB? (int)rhs.GetNumRows(): (int)rhs.GetNumCols();

        assert (m>0 && k>0 && l>0 && n>0);  //converting from size_t to int may cause overflow
        assert (k == l);
        if (k != l) 
        {
            throw std::invalid_argument("CPUSparseMatrix::MultiplyAndWeightedAdd: The inner dimensions of a and b must match.");
        }

        if (c.GetNumRows() != m || c.GetNumCols() != n) 
        {
            c.Resize(m,n);
        }         

        if (beta == 0)
        {
            memset(c.GetArray(), 0, sizeof(ElemType) * c.GetNumElements());
        }
        else if (beta != 1)
        {
#pragma omp parallel for
            foreach_coord(i,j,c)
            {
                c(i,j) = beta * c(i,j); 
            } 
        }

        if (rhs.GetFormat() != matrixFormatSparseCSC)
            NOT_IMPLEMENTED;

        if (!transposeA && !transposeB)
        {
            for(size_t j = 0; j < rhs.GetNumCols(); j++) 
            {
                size_t start = rhs.m_compIndex[j];  //ColLocation
                size_t end = rhs.m_compIndex[j+1];
                for(size_t p = start; p < end; p++)
                { 
                    size_t i = rhs.m_unCompIndex[p]; //RowLocation
                    ElemType val = rhs.m_pArray[p];

                    for(size_t h = 0; h < lhs.GetNumRows(); h++)
                    {
                        c(h,j) += alpha * lhs(h, i)*val; 
                    }
                }
            }
        }
        else if (!transposeA && transposeB)
        {           
            for(size_t j = 0; j < rhs.GetNumCols(); j++)
            { 
                size_t start = rhs.m_compIndex[j];
                size_t end = rhs.m_compIndex[j + 1];

                for(size_t p = start; p < end; p++)
                { 
                    size_t i = rhs.m_unCompIndex[p];
                    ElemType val = rhs.m_pArray[p];
                    for(size_t h = 0; h < lhs.GetNumRows(); h++)
                    {                     
                        c(h, i) += alpha * lhs(h, j)*val;
                    }
                }
            }           
        }
        else if (transposeA && !transposeB)
        {
            NOT_IMPLEMENTED;
        }
        else 
        {
            NOT_IMPLEMENTED;
        }
    }

    //c = alpha * op(lhs) * op(rhs)
    template<class ElemType>
    void CPUSparseMatrix<ElemType>::MultiplyAndAdd(ElemType alpha, const CPUMatrix<ElemType>& lhs, const bool transposeA, 
        const CPUSparseMatrix<ElemType>& rhs, const bool transposeB, CPUSparseMatrix<ElemType>& c)
    {
        if (lhs.IsEmpty() || rhs.IsEmpty())
            throw std::logic_error("LeftMultiplyAndAdd:  one of the input matrix is empty.");

        size_t m = transposeA? (int)lhs.GetNumCols(): (int)lhs.GetNumRows();
        size_t k = transposeA? (int)lhs.GetNumRows(): (int)lhs.GetNumCols();
        size_t l = transposeB? (int)rhs.GetNumCols(): (int)rhs.GetNumRows();
        size_t n = transposeB? (int)rhs.GetNumRows(): (int)rhs.GetNumCols();

        assert (m>0 && k>0 && l>0 && n>0); m; n;  //converting from size_t to int may cause overflow
        assert (k == l);
        if (k != l) 
        {
            throw std::invalid_argument("CPUSparseMatrix::MultiplyAndAdd: The inner dimensions of a and b must match.");
        }

        c.Reset();

        if (!transposeA && !transposeB)
        {
            NOT_IMPLEMENTED;
        }
        else if (!transposeA && transposeB)
        {           
            if (rhs.GetFormat() != matrixFormatSparseCSC)
                NOT_IMPLEMENTED;

            //allocate enough memory
            c.SetFormat(matrixFormatSparseBlockCol);
            c.Resize(m, n, m*min(n, rhs.m_nz), true, false);

            map<size_t, size_t> w2Id;
            for(size_t j = 0; j < rhs.GetNumCols(); j++)
            { // j ranges over batches
                size_t start = rhs.m_compIndex[j];
                size_t end = rhs.m_compIndex[j+1];

                for(size_t p = start; p < end; p++) 
                { 
                    size_t i = rhs.m_unCompIndex[p]; //i ranges over words
                    ElemType val = rhs.m_pArray[p]; //1 for(i, j)

                    bool first = true;
                    if(w2Id.find(i) == w2Id.end()) 
                    {
                        size_t id = w2Id.size();
                        w2Id[i] = id;
                        c.m_blockIds[c.m_blockSize]=i;
                        c.m_blockSize++;
                    } 
                    else 
                    {
                        first = false;
                    }
                    size_t pos = w2Id[i] * lhs.GetNumRows();
                    for(size_t h = 0; h < lhs.GetNumRows(); h++) 
                    { // h range over hidden layer 
                        if(first == true) 
                        {
                            c.m_pArray[pos] = alpha*lhs(h, j)*val;
                        } else 
                        {
                            c.m_pArray[pos] += alpha*lhs(h, j)*val;
                        }
                        pos++;
                    }
                }
            }   
            c.m_nz = c.m_blockSize * m;

            if(c.m_nz > c.GetSizeAllocated()) 
            {
                throw std::logic_error("sparse matrix out of range.");
            }
            //c.SetFormat(matrixFormatSparseBlockCol);
        }
        else if (transposeA && !transposeB)
        {
            NOT_IMPLEMENTED;
        }
        else 
        {
            NOT_IMPLEMENTED;
        }
    }

    template<class ElemType>
    void CPUSparseMatrix<ElemType>::ScaleAndAdd(const ElemType alpha, const CPUSparseMatrix<ElemType>& lhs, CPUMatrix<ElemType>& rhs)
    {
        if (lhs.IsEmpty() || rhs.IsEmpty()) 
        {
            throw std::logic_error("ScaleAndAdd:  one of the input matrix is empty.");
        }

        if (lhs.GetNumRows() != rhs.GetNumRows() || lhs.GetNumCols() != rhs.GetNumCols()) 
        {
            throw std::invalid_argument("CPUSparseMatrix::ScaleAndAdd: The dimensions of a and b must match.");
        }

        if(lhs.GetFormat() == MatrixFormat::matrixFormatSparseCSC || lhs.GetFormat() == MatrixFormat::matrixFormatSparseCSR) 
        {
            size_t col_num = (lhs.m_format == MatrixFormat::matrixFormatSparseCSC) ? lhs.GetNumCols(): lhs.GetNumRows();
            for(size_t j = 0; j < col_num; j++) 
            {
                size_t start = lhs.m_compIndex[j];
                size_t end = lhs.m_compIndex[j + 1];
                for(size_t p = start; p < end; p++) 
                {
                    size_t i = lhs.m_unCompIndex[p];
                    ElemType val = lhs.m_pArray[p];
                    size_t r = (lhs.m_format == MatrixFormat::matrixFormatSparseCSC) ? i : j;
                    size_t c = (lhs.m_format == MatrixFormat::matrixFormatSparseCSC) ? j : i;
                    rhs(r, c) += alpha * val; 
                }
            }
        } 
        else if (lhs.m_format == MatrixFormat::matrixFormatSparseBlockCol || lhs.m_format == MatrixFormat::matrixFormatSparseBlockRow) 
        {
            for(size_t j = 0; j < lhs.m_blockSize; j++) 
            {
                size_t i = lhs.m_blockIds[j];
                size_t len = (lhs.m_format == MatrixFormat::matrixFormatSparseBlockCol) ? lhs.GetNumRows() : lhs.GetNumCols();
                size_t start = j * len;
                for(size_t p = start; p < start+len; p++) 
                {
                    ElemType val = lhs.m_pArray[p];

                    size_t r = (lhs.m_format == MatrixFormat::matrixFormatSparseBlockCol) ? (p - start) : i;
                    size_t c = (lhs.m_format == MatrixFormat::matrixFormatSparseBlockCol) ? i : (p - start);
                    rhs(r, c) += alpha * val; 
                }
            }
        } 
        else 
        {
            throw std::runtime_error("CPUSparseMatrix:: ScaleAndAdd() Not implemented");
        }
    }


    template<class ElemType>
    bool CPUSparseMatrix<ElemType>::AreEqual(const CPUSparseMatrix<ElemType>& a, const CPUSparseMatrix<ElemType>& b, const ElemType threshold)
    {
        if (a.IsEmpty() || b.IsEmpty())
            throw std::logic_error("AreEqual: one of the input matrices is empty.");

        if (a.GetNumRows() != b.GetNumRows() || a.GetNumCols() != b.GetNumCols())
            return false;

        bool result = true;

        #pragma omp parallel for
        foreach_coord(i, j, a)
        {
            if (abs(a(i, j) - b(i, j)) > threshold)
            {
                result = false;
                break;
            }
        }

        return result;
    }

    // normal update for smoothed gradients c and current gradients (this)
    template<class ElemType> 
    void CPUSparseMatrix<ElemType>::NormalGrad(CPUMatrix<ElemType>& c, const ElemType momentum)
    {
        if (c.IsEmpty())
        {
            c.Resize(GetNumRows(), GetNumCols());
            c.SetValue(0.0);
        }

        if(m_format == MatrixFormat::matrixFormatSparseBlockCol || m_format == MatrixFormat::matrixFormatSparseBlockRow) 
        {
            for(size_t j = 0; j < m_blockSize; j++) 
            {
                size_t i = m_blockIds[j];
                size_t len = (m_format == MatrixFormat::matrixFormatSparseBlockCol) ? GetNumRows() : GetNumCols();
                size_t start = j* len;
                for(size_t p = start; p < start+len; p++) 
                {
                    ElemType val = m_pArray[p];
                    size_t row = (m_format == MatrixFormat::matrixFormatSparseBlockCol) ? (p - start) : i;
                    size_t col = (m_format == MatrixFormat::matrixFormatSparseBlockCol) ? i : (p - start);
                    c(row, col) = (1-momentum)*val + momentum*c(row, col);
                    m_pArray[p] = c(row, col);
                }
            }
        } 
        else 
        {
            throw std::runtime_error("CPUSparseMatrix:: NormalGrad() only support block sparse format");
        }
    }

    // update smoothed gradients c and current gradients (this)
    template<class ElemType> 
    ElemType CPUSparseMatrix<ElemType>::Adagrad(CPUMatrix<ElemType>& c, const bool needAveMultiplier)
    {
        if (c.IsEmpty() || c.GetNumCols() != GetNumCols() || c.GetNumRows() != GetNumRows())
        {
            c.Resize(GetNumRows(), GetNumCols());
            c.SetValue(0.0);
        }

        ElemType aveMultiplier = 0;

        const ElemType floor = 1e-16f;
        if(m_format == MatrixFormat::matrixFormatSparseCSC || m_format == MatrixFormat::matrixFormatSparseCSR) 
        {
            size_t col_num = (m_format == MatrixFormat::matrixFormatSparseCSC) ? GetNumCols() : GetNumRows();
            for(size_t j = 0; j < col_num; j++) 
            {
                size_t start = m_compIndex[j];
                size_t end = m_compIndex[j+1];
                for(size_t p = start; p < end; p++) 
                {
                    size_t i = m_unCompIndex[p];
                    ElemType val = m_pArray[p];

                    size_t row = (m_format == MatrixFormat::matrixFormatSparseCSC) ? i : j;
                    size_t col = (m_format == MatrixFormat::matrixFormatSparseCSC) ? j : i;
                    ElemType adenorm = c(row, col); 
                    adenorm += val * val; 
                    ElemType a = sqrt(floor + adenorm);
                    m_pArray[p] = val / a;
                    c(row, col) = adenorm; 

                    if (needAveMultiplier)
                        aveMultiplier += 1 / a;
                }
            }
        } 
        else if(m_format == MatrixFormat::matrixFormatSparseBlockCol || m_format == MatrixFormat::matrixFormatSparseBlockRow) 
        {
            size_t len = (m_format == MatrixFormat::matrixFormatSparseBlockCol) ? GetNumRows() : GetNumCols();
            size_t p = 0;
            for (long j = 0; j < m_blockSize; j++)
            {
                size_t colOrRow = m_blockIds[j];
                for (long i = 0; i < len; i++, p++) 
                {
                    ElemType val = m_pArray[p];

                    size_t row = (m_format == MatrixFormat::matrixFormatSparseBlockCol) ? i : colOrRow;
                    size_t col = (m_format == MatrixFormat::matrixFormatSparseBlockCol) ? colOrRow : i;
                    c(row, col) += val * val;
                    ElemType a = sqrt(floor + c(row, col));
                    m_pArray[p] /= a;

                    if (needAveMultiplier)
                        aveMultiplier += 1 / a;
                }
            }
        } 

        if (needAveMultiplier && m_nz > 0)
            return aveMultiplier / m_nz;
        else
            return 1;
    }

    template<class ElemType>
    CPUSparseMatrix<ElemType>& CPUSparseMatrix<ElemType>::InplaceTruncateTop(const ElemType threshold)
    {
        long m = (long)this->NzCount();
        ElemType *nzValues = NzValues();

#pragma omp parallel for     
        for (long i = 0; i<(m & ~3); i += 4)  //four-way unrolling
        {
            if (nzValues[i] > threshold)
                nzValues[i] = threshold;

            if (nzValues[i+1] > threshold)
                nzValues[i+1] = threshold;

            if (nzValues[i+2] > threshold)
                nzValues[i+2] = threshold;

            if (nzValues[i+3] > threshold)
                nzValues[i+3] = threshold;

        }
        //handle remaining stuffs
        for (long i = m & ~3; i<m; i++)
        {
            if (nzValues[i] > threshold)
                nzValues[i] = threshold;
        }

        return *this;
    }

    template<class ElemType>
    CPUSparseMatrix<ElemType>& CPUSparseMatrix<ElemType>::InplaceTruncateBottom(const ElemType threshold)
    {
        long m = (long)this->NzCount();
        ElemType *nzValues = NzValues();

#pragma omp parallel for     
        for (long i = 0; i<(m & ~3); i += 4)  //four-way unrolling
        {
            if (nzValues[i] < threshold)
                nzValues[i] = threshold;

            if (nzValues[i + 1] < threshold)
                nzValues[i + 1] = threshold;

            if (nzValues[i + 2] < threshold)
                nzValues[i + 2] = threshold;

            if (nzValues[i + 3] < threshold)
                nzValues[i + 3] = threshold;

        }
        //handle remaining stuffs
        for (long i = m & ~3; i<m; i++)
        {
            if (nzValues[i] < threshold)
                nzValues[i] = threshold;
        }

        return *this;
    }

    template<class ElemType>
    CPUSparseMatrix<ElemType>& CPUSparseMatrix<ElemType>::InplaceTruncate (const ElemType threshold)
    {
        ElemType locThresholdPos = abs(threshold);
        ElemType locTHresholdNeg = -locThresholdPos; 

        long m = (long)this->NzCount();
        ElemType *nzValues = NzValues();

#pragma omp parallel for     
        for (long i = 0; i<(m & ~3); i += 4)  //four-way unrolling
        {
            if (nzValues[i] > locThresholdPos)
                nzValues[i] = locThresholdPos;
            else if (nzValues[i] < locTHresholdNeg)
                nzValues[i] = locTHresholdNeg;

            if (nzValues[i+1] > locThresholdPos)
                nzValues[i+1] = locThresholdPos;
            else if (nzValues[i+1] < locTHresholdNeg)
                nzValues[i+1] = locTHresholdNeg;

            if (nzValues[i+2] > locThresholdPos)
                nzValues[i+2] = locThresholdPos;
            else if (nzValues[i+2] < locTHresholdNeg)
                nzValues[i+2] = locTHresholdNeg;

            if (nzValues[i+3] > locThresholdPos)
                nzValues[i+3] = locThresholdPos;
            else if (nzValues[i+3] < locTHresholdNeg)
                nzValues[i+3] = locTHresholdNeg;
        }
        //handle remaining stuffs
        for (long i = m & ~3; i<m; i++)
        {
            if (nzValues[i] > locThresholdPos)
                nzValues[i] = locThresholdPos;
            else if (nzValues[i] < locTHresholdNeg)
                nzValues[i] = locTHresholdNeg;
        }

        return *this;
    }    

    template<class ElemType>
    CPUSparseMatrix<ElemType>& CPUSparseMatrix<ElemType>::InplaceSoftThreshold(const ElemType threshold)
    {
        long m = (long)this->NzCount();
        ElemType *nzValues = NzValues();

#pragma omp parallel for     
        for (long i = 0; i<(m & ~3); i += 4)  //four-way unrolling
        {
            if (nzValues[i] > threshold)
                nzValues[i] -= threshold;
            else if (nzValues[i] < -threshold)
                nzValues[i] += threshold;
            else
                nzValues[i] = 0;

            if (nzValues[i + 1] > threshold)
                nzValues[i + 1] -= threshold;
            else if (nzValues[i + 1] < -threshold)
                nzValues[i + 1] += threshold;
            else
                nzValues[i + 1] = 0;

            if (nzValues[i + 2] > threshold)
                nzValues[i + 2] -= threshold;
            else if (nzValues[i + 2] < -threshold)
                nzValues[i + 2] += threshold;
            else
                nzValues[i + 2] = 0;

            if (nzValues[i + 3] > threshold)
                nzValues[i + 3] -= threshold;
            else if (nzValues[i + 3] < -threshold)
                nzValues[i + 3] += threshold;
            else
                nzValues[i + 3] = 0;
        }
        //handle remaining stuffs
        for (long i = m & ~3; i<m; i++)
        {
            if (nzValues[i] > threshold)
                nzValues[i] -= threshold;
            else if (nzValues[i] < -threshold)
                nzValues[i] += threshold;
            else
                nzValues[i] = 0;
        }
        return *this;
    }

    template<class ElemType>
    ElemType CPUSparseMatrix<ElemType>::FrobeniusNorm() const
    {
        if (this->IsEmpty())
            throw std::logic_error("FrobeniusNorm: Matrix is empty.");

        ElemType v = 0;

        long m = (long)this->NzCount();
        const ElemType *nzValues = NzValues();

        //four-way unrolling
#pragma omp parallel for reduction(+:v)
        for (long i = 0; i<(m & ~3); i += 4)
        {
            v += nzValues[i] * nzValues[i] + nzValues[i + 1] * nzValues[i + 1] + nzValues[i + 2] * nzValues[i + 2] + nzValues[i + 3] * nzValues[i + 3];
        }
        //handle remaining stuffs
        for (long i = m & ~3; i<m; i++)
        {
            v += nzValues[i] * nzValues[i];
        }

        return sqrt(v);
    }

    //sum of all abs(elements)
    template<class ElemType>
    ElemType CPUSparseMatrix<ElemType>::SumOfAbsElements() const
    {
        if (this->IsEmpty())
            throw std::logic_error("SumOfAbsElements: Matrix is empty.");

        if (sizeof(ElemType) == sizeof(double))
        {
#ifndef USE_MKL
            return (ElemType)dasum((int)this->NzCount(), reinterpret_cast <double*>(m_pArray), 1);
#else  
            return (ElemType)cblas_dasum((int)this->NzCount(), reinterpret_cast <double*>(m_pArray), 1);
#endif
        }
        else
        {
#pragma warning (suppress: 4244)
#ifndef USE_MKL
            return sasum((int)this->NzCount(), reinterpret_cast <float*>(m_pArray), 1);
#else
            return cblas_sasum((int)this->NzCount(), reinterpret_cast <float*>(m_pArray), 1);
#endif
        }
    }


    //sum of all elements
    template<class ElemType>
    ElemType CPUSparseMatrix<ElemType>::SumOfElements() const
    {
        if (this->IsEmpty())
            throw std::logic_error("SumOfElements: Matrix is empty.");

        ElemType sum = 0;

        long m = (long)this->NzCount();
        const ElemType *nzValues = NzValues();

        //four-way unrolling
#pragma omp parallel for reduction(+:sum)
        for (long i = 0; i<(m & ~3); i += 4)
        {
            sum += nzValues[i] + nzValues[i + 1] + nzValues[i + 2] + nzValues[i + 3];
        }
        //handle remaining stuffs
        for (long i = m & ~3; i<m; i++)
        {
            sum += nzValues[i];
        }

        return sum;
    }

    template <class ElemType>
    MATH_API File& operator>>(File& stream, CPUSparseMatrix<ElemType>& us)
    {
        stream.GetMarker(fileMarkerBeginSection, std::wstring(L"BMAT"));
        size_t elsize;
        stream >> elsize;
        if (sizeof(ElemType) != elsize)
            throw std::runtime_error("Template argument size doesn't match those in file");
        std::wstring matrixName;

        // now prepare this header to receive the data being read
        size_t nz, colnum, rownum;
        int format;

        // read in the header information
        stream >> matrixName >> format >> nz >> colnum >> rownum;

        us.SetFormat((MatrixFormat)format);
        if (us.GetFormat() != matrixFormatSparseCSC && us.GetFormat() != matrixFormatSparseCSR)
            NOT_IMPLEMENTED;

        us.Resize(rownum, colnum, nz, true, false);

        if (nz > 0)
        {
            size_t compressedSize = (us.GetFormat() == matrixFormatSparseCSC) ? colnum + 1 : rownum + 1;
            ElemType* dataBuffer = us.NzValues();
            CPUSPARSE_INDEX_TYPE* unCompressedIndex = us.MajorIndexLocation();
            CPUSPARSE_INDEX_TYPE* compressedIndex = us.SecondaryIndexLocation();

            // read in the sparse matrix info
            for (size_t i = 0; i < nz; ++i)
            {
                stream >> dataBuffer[i];
            }
            for (size_t i = 0; i < nz; ++i)
            {
                stream >> unCompressedIndex[i];
            }
            for (size_t i = 0; i < compressedSize; ++i)
            {
                stream >> compressedIndex[i];
            }
        }
        stream.GetMarker(fileMarkerEndSection, std::wstring(L"EMAT"));

        us.SetMatrixName(matrixName.c_str());

        return stream;
    }

    template MATH_API File& operator>>(File& stream, CPUSparseMatrix<float>& us);
    template MATH_API File& operator>>(File& stream, CPUSparseMatrix<double>& us);

    template <class ElemType>
    MATH_API File& operator<<(File& stream, const CPUSparseMatrix<ElemType>& us)
    {
        if (us.GetFormat() != matrixFormatSparseCSC && us.GetFormat() != matrixFormatSparseCSR)
            NOT_IMPLEMENTED;

        stream.PutMarker(fileMarkerBeginSection, std::wstring(L"BMAT"));
        stream << sizeof(ElemType);
        if (us.GetMatrixName() == nullptr)
        {
            std::wstring s(L"nnmatrix");
            stream << s;
        }
        else
        {
            stream << us.GetMatrixName();
        }

        size_t nz, numRows, numCols;
        size_t compressedSize = us.SecondaryIndexCount();
        int format = us.GetFormat();

        stream << format << nz << numCols << numRows;

        if (nz > 0)
        {
            ElemType* dataBuffer = us.NzValues();
            CPUSPARSE_INDEX_TYPE* unCompressedIndex = us.MajorIndexLocation();
            CPUSPARSE_INDEX_TYPE* compressedIndex = us.SecondaryIndexLocation();

            for (size_t i = 0; i < nz; ++i)
            {
                stream << dataBuffer[i];
            }
            for (size_t i = 0; i < nz; ++i)
            {
                stream << unCompressedIndex[i];
            }
            for (size_t i = 0; i < compressedSize; ++i)
            {
                stream << compressedIndex[i];
            }
        }
        stream.PutMarker(fileMarkerEndSection, std::wstring(L"EMAT"));

        return stream;
    }

    template class CPUSparseMatrix<float>;
    template class CPUSparseMatrix<double>;

}}}

/*
	Copyright (c) 2012 Christopher A. Taylor.  All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name of LibCat nor the names of its contributors may be used
	  to endorse or promote products derived from this software without
	  specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

/*
	TODO:

	2. Add diagnostic messages to all functions
	3. Update documentation for all functions

	4. Tune the matrix for larger sizes:
		+ Try higher-weight peeling codes
		+ Try different dense codes

	5. Implement partial input message
	6. Implement lookup table for codec parameters
	7. Generate lookup table

	8. Release party!
*/

/*
	Encoding Setup:

		T = Size of original data in bytes.
		N = ceil(T / M) = Count of blocks in the original data.

		(1) Generator Matrix Construction

			A = Original data blocks, N blocks long.
			H = Count of dense matrix rows (see below), chosen based on N.
			E = N + H blocks = Count of encoded data blocks.
			B = Encoded data blocks, E blocks long.
			G = Generator matrix, with E rows and E columns.

			+---------+---+   +---+   +---+
			|         |   |   |   |   |   |
			|    P    | M |   |   |   | A |
			|         |   | x | B | = |   |
			+---------+---+   |   |   +---+
			|    D    | I |   |   |   | 0 |
			+---------+---+   +---+   +---+

			A and B are Ex1 vectors of blocks.
				A has N rows of the original data padded by H zeroes.
				B has E rows of encoded blocks.

			G is the ExE binary matrix on the left.
				P is the NxN peeling matrix
					- Optimized for success of the peeling solver.
				M is the NxH mixing matrix
					- Used to mix the H dense rows into the N peeling rows.
				D is the HxN dense matrix
					- Used to improve recovery properties.
				I is the HxH identity matrix.

			G matrices for each value of N are precomputed offline and used
			based on the length of the input data, which guarantees that G
			is invertible.

		(2) Generating Matrix P

			The Hamming weight of each row of P is a random variable with a
			distribution chosen to optimize the operation of the peeling
			solver (see below).
			For each row of the matrix, this weight is determined and 1 bits
			are then uniformly distributed over the N columns.

		(3) Generating Matrix M

			Rows of M are generated with a constant weight of 2 and 1 bits are
			uniformly distributed over the H columns.

		(4) Generating Matrix D

			Each bit has a 50% chance of being set.

		(5) Generator Matrix Inversion

			An optimized sparse inversion technique is used to solve for B.

	---------------------------------------------------------------------------
	Sparse Matrix Inversion:

		There are 4 phases to this sparse inversion:

		(1) Peeling
			- Opportunistic fast solution for first N rows.
		(2) Compression
			- Setup for Gaussian elimination on a wide rectangular matrix
		(3) Gaussian Elimination
			- Gaussian elimination on a (hopefully) small square matrix
		(4) Substitution
			- Solves for remaining rows from initial peeling

	(1) Peeling:

		Until N rows are received, the peeling algorithm is executed:

		Columns have 3 states:

		(1) Peeled - Solved by a row during peeling process.
		(2) Deferred - Will be solved by a row during Gaussian Elimination.
		(3) Unmarked - Still deciding.

		Initially all columns are unmarked.

		As a row comes in, the count of columns that are Unmarked is calculated.
	If that count is 1, then the column is marked as Peeled and is solved by
	the received row.  Peeling then goes through all other rows that reference
	that peeled column, reducing their count by 1, potentially causing other
	columns to be marked as Peeled.  This "peeling avalanche" is desired.

		After N rows, an unmarked column is marked Deferred and the count of all
	rows that reference that column are decremented by 1.  Peeling avalanche may
	occur as a result.  This process is repeated until all columns are marked.

	A comment on the matrix state after peeling:

		After the peeling solver has completed, only Deferred columns remain to be
	solved.  Conceptually the matrix can be re-ordered in the order of solution
	so that the matrix resembles this example:

		+-----+---------+-----+
		|   1 | 1       | 721 | <-- Peeled row 1
		| 1   |   1     | 518 | <-- Peeled row 2
		| 1   | 1   1   | 934 | <-- Peeled row 3
		|   1 | 1   1 1 | 275 | <-- Peeled row 4
		+-----+---------+-----+
		| 1   | 1 1   1 | 123 | <-- Deferred rows
		|   1 |   1 1 1 | 207 | <-- Deferred rows
		+-----+---------+-----+
		   ^       ^       ^---- Row value (unmodified so far, ie. 1500 bytes)
		   |       \------------ Peeled columns
		   \-------------------- Deferred columns

		Re-ordering the actual matrix is not required, but the lower-triangular
	form of the peeled matrix is apparent in the diagram above.

	(2) Compression:

		The next step is to compress the GE rows and columns into a smaller GE
	matrix that GE will be applied to.  Note that up to this point, no row ops
	have been performed.  GE row values will be calculated in this step for use
	during GE.

		For each peeled column from the last to the first,
			For each GE row that contains that column,
				Add the corresponding peeled row to it.
			Next
		Next

		The resultant conceptual matrix may resemble this example:

		+-----+---------+-----+
		|   1 | 0 0 0 0 | 285 |
		| 1 1 | 0 0 0 0 | 712 |
		+-----+---------+-----+
		   ^       ^       ^---- Row values
		   |       \------------ Peeled matrix (rectangular)
		   \-------------------- GE square matrix

		The density of the rows is indicated in the example because it is worth
	considering.  Some of the rows may be low density, so mixing them before
	calculating the new row value is undesirable.  By calculating the row values
	at this point, row operations may be saved.

	(3) Gaussian Elimination:

		In this step the GE square matrix is put in upper-triangular form and
	initial GE square matrix row values are calculated.  Back-substitution
	will be performed afterwards to complete solving for the row values.

		For each GE matrix column from left to right,
			For each GE matrix row from top to bottom,
				If GE matrix bit is set,
					Set this row as the pivot for this column.

					For each remaining GE matrix row containing this column,
						Sum other GE square matrix rows with the pivot row,
							including the row values.
					Next
				End
			Next

			If no pivot was found for this column,
				The process fails and needs to resume on the next block.
			End
		Next

		At successful completion of this process, all pivots have been found.
	The GE square matrix is now in upper triangular form.

		To complete solving for the GE row values, the upper triangular part
	of the GE square matrix must be eliminated.  This process will diagonalize
	the GE square matrix, conceptually leaving it as an identity matrix.

		For each pivot column from last to first,
			For each pivot row above it,
				If the pivot row contains the column,
					Add the pivot column value to the pivot row value
				End
			Next
		Next

		Note that the GE square matrix is no longer needed and the pivot row
	values are entirely determined.  Example result:

		+-----+
		| 001 | <- Pivot 1
		| 002 | <- Pivot 2
		+-----+

	(4) Substitution:

		Finally the peeled row values may be determined by regenerating the
	whole matrix and summing up columns in the original order of peeling.

		For each peeled row from first to last,
			For each active column in the row,
				Add corresponding row value to peeled row.
			Next
		Next

	Review:

		After all of these steps, the row values have been determined and the
	matrix inversion is complete.  Let's analyze the complexity of each step:

		(1) Peeling
			- Opportunistic fast solution for first N rows.

			Weight determination : O(k) average
				Column reference update : Amortized O(1) for each column

			If peeling activates,
				Marking as peeled : O(1)

				Reducing weight of rows referencing this column : O(k)
					If other row weight is reduced to 2,
						Regenerate columns and mark potential Deferred : O(k)
					End
			End

			So peeling is O(1) for each row, and O(N) overall.

		(2) Compression
			- Setup for Gaussian elimination on a wide rectangular matrix

			Summing peeled rows into GE rows : O(k*N*N) bit operations

			This algorithm is fairly complex in terms of real-world time
			spent calculating it.  I expect about 10% of the run time to
			be spent executing this code.  However the total execution
			time is dominated by the number of row sum operations (large
			XOR operations) so this non-linear asymptotic performance is
			not a problem.

			Summing to produce the row values : O(S*N) = O(N^1.5) row ops

		(3) Gaussian Elimination
			- Gaussian elimination on a (hopefully) small square matrix

			Assume the GE square matrix is SxS, and S = sqrt(N) on
			average thanks to the peeling solver above.

			Gaussian elimination : O(S^3) = O(N^1.5) bit operations

			This algorithm is not bad because the matrix is small and
			it really doesn't contribute much to the run time.

			- Solves for rows of small square matrix

			Assume the GE square matrix is SxS, and S = sqrt(N) on
			average thanks to the peeling solver above.

			Back-substitute inside the GE matrix : O(S^2) = O(N) row ops

		(4) Substitution
			- Solves for remaining rows from initial peeling

			Regenerate peeled matrix rows and substitute : O(N*k) row ops

		As compared with matrix multiplication, this algorithm has:

		+ Memory overhead for the peeling solver state.
		+ O(N^2) bit operations for Compression.
		+ O(N + N^1.5) heavy row ops for GE.

		It gains back a little because it is not spending additional row ops
		to solve row values involved in GE.  The final substitution step is
		the same as matrix multiplication for those rows (same as the encoder).

	---------------------------------------------------------------------------

	Encoding:

			The first N output blocks of the encoder are the same as the
		original data.  After that the encoder will start producing random-
		looking M-byte blocks by generating new rows for P and M and
		multiplying them by B.

	Decoding:

			Decoding begins by collecting N blocks from the transmitter.  Once
		N blocks are received, the matrix G' (differing in the first N rows
		from the above matrix G) is generated with the rows of P|M that were
		received.  Generator matrix inversion is attempted, failing at the
		Gaussian elimination step if a pivot cannot be found for one of the GE
		matrix columns (see above).

			New rows are received and submitted directly to the GE solver,
		hopefully providing the missing pivot.  Once enough rows have been
		received, back-substitution reconstructs matrix B.
		
			The first N rows of the original matrix G are then used to fill in
		any blocks that were not received from the original N blocks, and the
		original data is received.
*/

#include "Wirehair.hpp"
#include "memxor.hpp"
using namespace cat;
using namespace wirehair;

#if defined(CAT_DUMP_CODEC_DEBUG)
#define CAT_IF_DUMP(x) x
#else
#define CAT_IF_DUMP(x)
#endif

#if defined(CAT_DUMP_ROWOP_COUNTERS)
#define CAT_IF_ROWOP(x) x
#else
#define CAT_IF_ROWOP(x)
#endif

#if defined(CAT_DUMP_CODEC_DEBUG) || defined(CAT_DUMP_ROWOP_COUNTERS) || defined(CAT_DUMP_GE_MATRIX)
#include <iostream>
#include <iomanip>
#include <fstream>
using namespace std;
#endif


//// Utility: Get Result String function

const char *cat::wirehair::GetResultString(Result r)
{
	switch (r)
	{
	case R_WIN:				return "R_WIN";
	case R_MORE_BLOCKS:		return "R_MORE_BLOCKS";
	case R_BAD_INPUT:		return "R_BAD_INPUT";
	case R_OUT_OF_MEMORY:	return "R_OUT_OF_MEMORY";
	default:				return "UnrecognizedResultCode";
	}
}


//// Utility: 16-bit Integer Square Root function

/*
	Based on code from http://www.azillionmonkeys.com/qed/sqroot.html

		Contributors include Arne Steinarson for the basic approximation idea, 
		Dann Corbit and Mathew Hendry for the first cut at the algorithm, 
		Lawrence Kirby for the rearrangement, improvments and range optimization
		and Paul Hsieh for the round-then-adjust idea.

	I tried this out, stdlib sqrt() and a few variations on Newton-Raphson
	and determined this one is, by far, the fastest.  I adapted it to 16-bit
	input for additional performance and tweaked the operations to work best
	with the MSVC optimizer, which turned out to be very sensitive to the
	way that the code is written.
*/
static const u8 SQQ_TABLE[] = {
	0,  16,  22,  27,  32,  35,  39,  42,  45,  48,  50,  53,  55,  57,
	59,  61,  64,  65,  67,  69,  71,  73,  75,  76,  78,  80,  81,  83,
	84,  86,  87,  89,  90,  91,  93,  94,  96,  97,  98,  99, 101, 102,
	103, 104, 106, 107, 108, 109, 110, 112, 113, 114, 115, 116, 117, 118,
	119, 120, 121, 122, 123, 124, 125, 126, 128, 128, 129, 130, 131, 132,
	133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 144, 145,
	146, 147, 148, 149, 150, 150, 151, 152, 153, 154, 155, 155, 156, 157,
	158, 159, 160, 160, 161, 162, 163, 163, 164, 165, 166, 167, 167, 168,
	169, 170, 170, 171, 172, 173, 173, 174, 175, 176, 176, 177, 178, 178,
	179, 180, 181, 181, 182, 183, 183, 184, 185, 185, 186, 187, 187, 188,
	189, 189, 190, 191, 192, 192, 193, 193, 194, 195, 195, 196, 197, 197,
	198, 199, 199, 200, 201, 201, 202, 203, 203, 204, 204, 205, 206, 206,
	207, 208, 208, 209, 209, 210, 211, 211, 212, 212, 213, 214, 214, 215,
	215, 216, 217, 217, 218, 218, 219, 219, 220, 221, 221, 222, 222, 223,
	224, 224, 225, 225, 226, 226, 227, 227, 228, 229, 229, 230, 230, 231,
	231, 232, 232, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238,
	239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246,
	246, 247, 247, 248, 248, 249, 249, 250, 250, 251, 251, 252, 252, 253,
	253, 254, 254, 255
};

u16 cat::wirehair::SquareRoot16(u16 x)
{
	u16 r;

	if (x >= 0x100)
	{
		if (x >= 0x1000)
		{
			if (x >= 0x4000)
				r = SQQ_TABLE[x >> 8] + 1;
			else
				r = (SQQ_TABLE[x >> 6] >> 1) + 1;
		}
		else
		{
			if (x >= 0x400)
				r = (SQQ_TABLE[x >> 4] >> 2) + 1;
			else
				r = (SQQ_TABLE[x >> 2] >> 3) + 1;
		}
	}
	else
	{
		return SQQ_TABLE[x] >> 4;
	}

	// Correct rounding if necessary (compiler optimizes this form better)
	r -= (r * r > x);

	return r;
}


//// Utility: 16-bit Truncated Sieve of Eratosthenes Next Prime function

/*
	It uses trial division up to the square root of the number to test.
	Uses a truncated sieve table to pick the next number to try, which
	avoids small factors 2, 3, 5, 7.  This can be considered a more
	involved version of incrementing by 2 instead of 1.  It takes about
	25% less time on average than the approach that just increments by 2.

	Because of the tabular increment this is a hybrid approach.  The sieve
	would just use a very large table, but I wanted to limit the size of
	the table to something fairly small and reasonable.  210 bytes for the
	sieve table.  102 bytes for the primes list.

	It also calculates the integer square root faster than cmath sqrt()
	and uses multiplication to update the square root instead for speed.
*/
const int SIEVE_TABLE_SIZE = 2*3*5*7;
static const u8 SIEVE_TABLE[SIEVE_TABLE_SIZE] = {
	1, 0, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0,
	1, 0, 5, 4, 3, 2, 1, 0, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0,
	1, 0, 5, 4, 3, 2, 1, 0, 3, 2, 1, 0, 1, 0, 5, 4, 3, 2, 1, 0, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0,
	7, 6, 5, 4, 3, 2, 1, 0, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 7, 6, 5, 4, 3, 2,
	1, 0, 5, 4, 3, 2, 1, 0, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0,
	1, 0, 5, 4, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 5, 4, 3, 2, 1, 0,
	1, 0, 5, 4, 3, 2, 1, 0, 3, 2, 1, 0, 1, 0, 3, 2, 1, 0, 1, 0, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
};

static const u16 PRIMES_UNDER_256[] = {
	11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61,
	67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127,
	131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191,
	193, 197, 199, 211, 223, 227, 229, 233, 239, 241, 251, 0x7fff
};

u16 cat::wirehair::NextPrime16(u16 n)
{
	// Handle small n
	switch (n)
	{
	case 0:
	case 1:	return 1;
	case 2:	return 2;
	case 3:	return 3;
	case 4:
	case 5:	return 5;
	case 6:
	case 7:	return 7;
	}

	// Choose first n from table
	int offset = n % SIEVE_TABLE_SIZE;
	u32 next = SIEVE_TABLE[offset];
	offset += next + 1;
	n += next;

	// Initialize p_max to sqrt(n)
	int p_max = SquareRoot16(n);

	// For each number to try,
	for (;;)
	{
		// For each prime to test up to square root,
		const u16 *prime = PRIMES_UNDER_256;
		for (;;)
		{
			// If the next prime is above p_max we are done!
			int p = *prime;
			if (p > p_max)
				return n;

			// If composite, try next n
			if (n % p == 0)
				break;

			// Try next prime
			++prime;
		}

		// Use table to choose next trial number
		if (offset >= SIEVE_TABLE_SIZE) offset -= SIEVE_TABLE_SIZE;
		u32 next = SIEVE_TABLE[offset];
		offset += next + 1;
		n += next + 1;

		// Derivative square root iteration of p_max
		if (p_max * p_max < n)
			++p_max;
	}

	return n;
}


//// Utility: Peeling Row Weight Generator function

static const u32 WEIGHT_DIST[] = {
	0, 5243, 529531, 704294, 791675, 844104, 879057, 904023,
	922747, 937311, 948962, 958494, 966438, 973160, 978921,
	983914, 988283, 992138, 995565, 998631, 1001391, 1003887,
	1006157, 1008229, 1010129, 1011876, 1013490, 1014983,
	1016370, 1017662, 1048576
};

u16 cat::wirehair::GeneratePeelRowWeight(u32 rv, u16 max_weight)
{
	rv &= 0xfffff;

	u16 ii;
	for (ii = 1; rv >= WEIGHT_DIST[ii]; ++ii);

	return ii > max_weight ? max_weight : ii;
}


//// Utility: GF(2) Invertible Matrix Generator function

/*
	Sometimes it is helpful to be able to quickly generate a GF2 matrix
	that is invertible.  I guess.  Anyway, here's a lookup table of
	seeds that create invertible GF2 matrices and a function that will
	fill a bitfield with the matrix.

	The function generates random-looking invertible matrices for
		0 < N < 512
	And for larger values of N it will just add the identity matrix.

	It will add the generated matrix rather than overwrite what was there.

	It's a little messy (will write random bits past the end of the
	matrix padded out to the end of the last word).  But the way I
	am using it, this is acceptable.  If you want to use it too be
	sure to verify this is not a problem for you...
*/

static const u8 INVERTIBLE_MATRIX_SEEDS[512] = {
	0x0,0,2,2,10,5,6,1,2,0,0,3,5,0,0,1,0,0,0,3,0,1,2,3,0,1,6,6,1,6,0,0,
	0,4,2,7,0,2,4,2,1,1,0,0,2,12,11,3,3,3,2,1,1,4,4,1,13,2,2,1,3,2,1,1,
	3,1,0,0,1,0,0,10,8,6,0,7,3,0,1,1,0,2,6,3,2,2,1,0,5,2,5,1,1,2,4,1,
	2,1,0,0,0,2,0,5,9,17,5,1,2,2,5,4,4,4,4,4,1,2,2,2,1,0,1,0,3,2,2,0,
	1,4,1,3,1,17,3,0,0,0,0,2,2,0,0,0,1,11,4,2,4,2,1,8,2,1,1,2,6,3,0,4,
	3,10,5,3,3,1,0,1,2,6,10,10,6,0,0,0,0,0,0,1,4,2,1,2,2,12,2,2,4,0,0,2,
	0,7,12,1,1,1,0,6,8,0,0,0,0,2,1,8,6,2,0,5,4,2,7,2,10,4,2,6,4,6,6,1,
	0,0,0,0,3,1,0,4,2,6,1,1,4,2,5,1,4,1,0,0,1,8,0,0,6,0,17,4,9,8,4,4,
	3,0,0,3,1,4,3,3,0,0,3,0,0,0,3,4,4,4,3,0,0,12,1,1,2,5,8,4,8,6,2,2,
	0,0,0,13,0,3,4,2,2,1,6,13,3,12,0,0,3,7,8,2,2,2,0,0,4,0,0,0,2,0,3,6,
	7,1,0,2,2,4,4,3,6,3,6,4,4,1,3,7,1,0,0,0,1,3,0,5,4,4,4,3,1,1,7,13,
	4,6,1,1,2,2,2,5,7,1,0,0,2,2,1,2,1,6,6,6,2,2,2,5,3,2,0,0,0,0,0,0,
	0,0,2,3,2,2,0,4,0,0,4,2,0,0,0,2,4,1,2,3,1,1,1,1,1,1,1,1,4,0,0,0,
	1,1,0,0,0,0,0,4,3,0,0,0,0,4,0,0,4,5,2,0,1,0,0,1,7,1,0,0,0,0,1,1,
	1,6,3,0,0,1,3,2,0,3,0,2,1,1,1,0,0,0,0,0,0,8,0,0,6,4,1,3,5,3,0,1,
	1,6,3,3,5,2,2,9,5,1,2,2,1,1,1,1,1,1,2,2,1,3,1,0,0,4,1,7,0,0,0,0,
};

bool cat::wirehair::AddInvertibleGF2Matrix(u64 *matrix, int offset, int pitch, int n)
{
	if (n <= 0) return false;

	// If we have this value of n in the table,
	if (n < 512)
	{
		// Pull a random matrix out of the lookup table
		CatsChoice prng;
		prng.Initialize(INVERTIBLE_MATRIX_SEEDS[n]);

		// If shift is friendly,
		u32 shift = offset & 63;
		u64 *row = matrix + (offset >> 6);
		if (shift > 0)
		{
			// For each row,
			for (int row_i = 0; row_i < n; ++row_i, row += pitch)
			{
				// For each word in the row,
				int add_pitch = (n + 63) / 64;
				u64 prev = 0;
				for (int ii = 0; ii < add_pitch; ++ii)
				{
					// Generate next word
					u32 rv1 = prng.Next();
					u32 rv2 = prng.Next();
					u64 word = ((u64)rv2 << 32) | rv1;

					// Add it in
					row[ii] ^= (prev >> (64 - shift)) | (word << shift);

					prev = word;
				}

				// Add last word if needed
				int last_bit = (shift + n + 63) / 64;
				if (last_bit > add_pitch)
					row[add_pitch] ^= prev >> (64 - shift);
			}
		}
		else // Rare aligned case:
		{
			// For each row,
			for (int row_i = 0; row_i < n; ++row_i, row += pitch)
			{
				// For each word in the row,
				for (int add_pitch = (n + 63) / 64, ii = 0; ii < add_pitch; ++ii)
				{
					// Generate next word
					u32 rv1 = prng.Next();
					u32 rv2 = prng.Next();
					u64 word = ((u64)rv2 << 32) | rv1;

					// Add it in
					row[ii] ^= word;
				}
			}
		}
	}
	else
	{
		// For each row,
		u64 *row = matrix;
		for (int ii = 0; ii < n; ++ii, row += pitch)
		{
			// Flip diagonal bit
			int column_i = offset + ii;
			row[column_i >> 6] ^= (u64)1 << (column_i & 63);
		}
	}

	return true;
}


//// Utility: Deck Shuffling function

/*
	Given a PRNG, generate a deck of cards in a random order.
	The deck will contain elements with values between 0 and count - 1.
*/

void cat::wirehair::ShuffleDeck16(CatsChoice &prng, u16 *deck, u32 count)
{
	deck[0] = 0;

	// If we can unroll 4 times,
	if (count <= 256)
	{
		for (u32 ii = 1;;)
		{
			u32 jj, rv = prng.Next();

			// 8-bit unroll
			switch (count - ii)
			{
			default:
				jj = (u8)rv % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
				jj = (u8)(rv >> 8) % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
				jj = (u8)(rv >> 16) % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
				jj = (u8)(rv >> 24) % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
				break;

			case 3:
				jj = (u8)rv % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
			case 2:
				jj = (u8)(rv >> 8) % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
			case 1:
				jj = (u8)(rv >> 16) % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
			case 0:
				return;
			}
		}
	}
	else
	{
		// For each deck entry,
		for (u32 ii = 1;;)
		{
			u32 jj, rv = prng.Next();

			// 16-bit unroll
			switch (count - ii)
			{
			default:
				jj = (u16)rv % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
				jj = (u16)(rv >> 16) % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
				++ii;
				break;

			case 1:
				jj = (u16)rv % ii;
				deck[ii] = deck[jj];
				deck[jj] = ii;
			case 0:
				return;
			}
		}
	}
}


//// Utility: Peel Matrix Row Generator function

void cat::wirehair::GeneratePeelRow(u32 id, u32 p_seed, u16 peel_column_count, u16 mix_column_count,
	u16 &peel_weight, u16 &peel_a, u16 &peel_x0, u16 &mix_a, u16 &mix_x0)
{
	// Initialize PRNG
	CatsChoice prng;
	prng.Initialize(id, p_seed);

	// Generate peeling matrix column parameters
	peel_weight = GeneratePeelRowWeight(prng.Next(), peel_column_count - 1);
	u32 rv = prng.Next();
	peel_a = ((u16)rv % (peel_column_count - 1)) + 1;
	peel_x0 = (u16)(rv >> 16) % peel_column_count;

	// Generate mixing matrix column parameters
	rv = prng.Next();
	mix_a = ((u16)rv % (mix_column_count - 1)) + 1;
	mix_x0 = (u16)(rv >> 16) % mix_column_count;
}


//// Utility: Matrix Parameter Generator function

int g_p_seed, g_c_seed; // TODO: Remove these

bool cat::wirehair::GenerateMatrixParameters(int block_count, u32 &p_seed, u32 &c_seed, u16 &light_count, u16 &dense_count)
{
	p_seed = g_p_seed;// TODO: Remove these
	c_seed = g_c_seed;

	switch (block_count)
	{
	case 16:
		light_count = 6; dense_count = 2;
		return true;
	case 64:
		light_count = 8; dense_count = 2;
		return true;
	case 128:
		light_count = 11; dense_count = 2;
		return true;
	case 256:
		light_count = 14; dense_count = 5;
		return true;
	case 512:
		light_count = 14;
		dense_count = 5;
		return true;
	case 1024:
		light_count = 18;
		dense_count = 12;
		return true;
	case 2048:
		light_count = 45;
		dense_count = 8;
		return true;
	case 4096:
		light_count = 55;
		dense_count = 14;
		return true;
	case 8192:
		light_count = 100;
		dense_count = 16;
		return true;
	case 10000:
		light_count = 120;
		dense_count = 20;
		return true;
	case 16384:
		light_count = 180;
		dense_count = 26;
		return true;
	case 32768:
		light_count = 400;
		dense_count = 30;
		return true;
	case 40000:
		light_count = 460;
		dense_count = 29;
		return true;
	case 50000:
		light_count = 600;
		dense_count = 34;
		return true;
	case 64000:
		light_count = 6;
		dense_count = 750;
		return true;
	}

	return false;
}


//// Data Structures

#pragma pack(push)
#pragma pack(1)
struct Codec::PeelRow
{
	u16 next;					// Linkage in row list
	u32 id;						// Identifier for this row

	// Peeling matrix: Column generator
	u16 peel_weight, peel_a, peel_x0;

	// Mixing matrix: Column generator
	u16 mix_a, mix_x0;

	// Peeling state
	u16 unmarked_count;			// Count of columns that have not been marked yet
	union
	{
		// During peeling:
		u16 unmarked[2];		// Final two unmarked column indices

		// After peeling:
		struct
		{
			u16 peel_column;	// Peeling column that is solved by this row
			u8 is_copied;		// Row value is copied yet?
		};
	};
};
#pragma pack(pop)

enum MarkTypes
{
	MARK_TODO,
	MARK_PEEL,
	MARK_DEFER
};

#pragma pack(push)
#pragma pack(1)
struct Codec::PeelColumn
{
	u16 next;			// Linkage in column list

	union
	{
		u16 w2_refs;	// Number of weight-2 rows containing this column
		u16 peel_row;	// Row that solves the column
		u16 ge_column;	// Column that a deferred column is mapped to
	};

	u8 mark;			// One of the MarkTypes enumeration
};
#pragma pack(pop)

#pragma pack(push)
#pragma pack(1)
struct Codec::PeelRefs
{
	u16 row_count;		// Number of rows containing this column
	u16 rows[CAT_REF_LIST_MAX];
};
#pragma pack(pop)


//// (1) Peeling:

/*
	Peel() and PeelAvalanche() are split up into two functions because I found
	that the PeelAvalanche() function can be reused later during GreedyPeeling().

		Until N rows are received, the peeling algorithm is executed:

		Columns have 3 states:

		(1) Peeled - Solved by a row during peeling process.
		(2) Deferred - Will be solved by a row during Gaussian Elimination.
		(3) Unmarked - Still deciding.

		Initially all columns are unmarked.

		As a row comes in, the count of columns that are Unmarked is calculated.
	If that count is 1, then the column is marked as Peeled and is solved by
	the received row.  Peeling then goes through all other rows that reference
	that peeled column, reducing their count by 1, potentially causing other
	columns to be marked as Peeled.  This "peeling avalanche" is desired.
*/

bool Codec::OpportunisticPeeling(u32 row_i, u32 id)
{
	PeelRow *row = &_peel_rows[row_i];

	row->id = id;
	GeneratePeelRow(id, _p_seed, _block_count, _added_count,
		row->peel_weight, row->peel_a, row->peel_x0, row->mix_a, row->mix_x0);

	CAT_IF_DUMP(cout << "Row " << id << " in slot " << row_i << " of weight " << row->peel_weight << " [a=" << row->peel_a << "] : ";)

	// Iterate columns in peeling matrix
	u16 weight = row->peel_weight;
	u16 column_i = row->peel_x0;
	u16 a = row->peel_a;
	u16 unmarked_count = 0;
	u16 unmarked[2];
	for (;;)
	{
		CAT_IF_DUMP(cout << column_i << " ";)

		PeelColumn *col = &_peel_cols[column_i];
		PeelRefs *refs = &_peel_col_refs[column_i];

		// Add row reference to column
		if (refs->row_count >= CAT_REF_LIST_MAX)
		{
			CAT_IF_DUMP(cout << "OpportunisticPeeling: Failure!  Ran out of space for row references.  CAT_REF_LIST_MAX must be increased!" << endl;)
			return false;
		}
		refs->rows[refs->row_count++] = row_i;

		// If column is unmarked,
		if (col->mark == MARK_TODO)
			unmarked[unmarked_count++ & 1] = column_i;

		if (--weight <= 0) break;

		IterateNextColumn(column_i, _block_count, _block_next_prime, a);
	}
	CAT_IF_DUMP(cout << endl;)

	// Initialize row state
	row->unmarked_count = unmarked_count;

	switch (unmarked_count)
	{
	case 0:
		// Link at head of defer list
		row->next = _defer_head_rows;
		_defer_head_rows = row_i;
		break;

	case 1:
		// Solve only unmarked column with this row
		Peel(row_i, row, unmarked[0]);
		break;

	case 2:
		// Remember which two columns were unmarked
		row->unmarked[0] = unmarked[0];
		row->unmarked[1] = unmarked[1];

		// Increment weight-2 reference count for unmarked columns
		_peel_cols[unmarked[0]].w2_refs++;
		_peel_cols[unmarked[1]].w2_refs++;
		break;
	}

	return true;
}

void Codec::PeelAvalanche(u16 column_i)
{
	// Walk list of peeled rows referenced by this newly solved column
	PeelRefs *refs = &_peel_col_refs[column_i];
	u16 ref_row_count = refs->row_count;
	u16 *ref_rows = refs->rows;
	while (ref_row_count--)
	{
		// Update unmarked row count for this referenced row
		u16 ref_row_i = *ref_rows++;
		PeelRow *ref_row = &_peel_rows[ref_row_i];
		u16 unmarked_count = --ref_row->unmarked_count;

		// If row may be solving a column now,
		if (unmarked_count == 1)
		{
			// Find other column
			u16 new_column_i = ref_row->unmarked[0];
			if (new_column_i == column_i)
				new_column_i = ref_row->unmarked[1];

			/*
				Rows that are to be deferred will either end up
				here or below where it handles the case of there
				being no columns unmarked in a row.
			*/

			// If column is already solved,
			if (_peel_cols[new_column_i].mark == MARK_TODO)
				Peel(ref_row_i, ref_row, new_column_i);
			else
			{
				CAT_IF_DUMP(cout << "PeelAvalanche: Deferred(1) with column " << column_i << " at row " << ref_row_i << endl;)

				// Link at head of defer list
				ref_row->next = _defer_head_rows;
				_defer_head_rows = ref_row_i;
			}
		}
		else if (unmarked_count == 2)
		{
			// Regenerate the row columns to discover which are unmarked
			u16 ref_weight = ref_row->peel_weight;
			u16 ref_column_i = ref_row->peel_x0;
			u16 ref_a = ref_row->peel_a;
			u16 unmarked_count = 0;
			for (;;)
			{
				PeelColumn *ref_col = &_peel_cols[ref_column_i];

				// If column is unmarked,
				if (ref_col->mark == MARK_TODO)
				{
					// Store the two unmarked columns in the row
					ref_row->unmarked[unmarked_count++] = ref_column_i;

					// Increment weight-2 reference count (cannot hurt even if not true)
					ref_col->w2_refs++;
				}

				if (--ref_weight <= 0) break;

				IterateNextColumn(ref_column_i, _block_count, _block_next_prime, ref_a);
			}

			/*
				This is a little subtle, but sometimes the avalanche will
				happen here, and sometimes a row will be marked deferred.
			*/

			if (unmarked_count <= 1)
			{
				// Insure that this row won't be processed further during this recursion
				ref_row->unmarked_count = 0;

				// If row is to be deferred,
				if (unmarked_count == 1)
					Peel(ref_row_i, ref_row, ref_row->unmarked[0]);
				else
				{
					CAT_IF_DUMP(cout << "PeelAvalanche: Deferred(2) with column " << column_i << " at row " << ref_row_i << endl;)

					// Link at head of defer list
					ref_row->next = _defer_head_rows;
					_defer_head_rows = ref_row_i;
				}
			}
		}
	}
}

void Codec::Peel(u16 row_i, PeelRow *row, u16 column_i)
{
	CAT_IF_DUMP(cout << "Peel: Solved column " << column_i << " with row " << row_i << endl;)

	PeelColumn *column = &_peel_cols[column_i];

	// Mark this column as solved
	column->mark = MARK_PEEL;

	// Remember which column it solves
	row->peel_column = column_i;

	// Link to back of the peeled list
	if (_peel_tail_rows)
		_peel_tail_rows->next = row_i;
	else
		_peel_head_rows = row_i;
	row->next = LIST_TERM;
	_peel_tail_rows = row;

	// Indicate that this row hasn't been copied yet
	row->is_copied = 0;

	// Attempt to avalanche and solve other columns
	PeelAvalanche(column_i);

	// Remember which row solves the column, after done with rows list
	column->peel_row = row_i;
}

/*
		After the opportunistic peeling solver has completed, no columns have
	been deferred to Gaussian elimination yet.  Greedy peeling will then take
	over and start choosing columns to defer.  It is greedy in that the selection
	of which columns to defer is based on a greedy initial approximation of the
	best column to choose, rather than the best one that could be chosen.

		In this algorithm, the column that will cause the largest immediate
	avalanche of peeling solutions is the one that is selected.  If there is
	a tie between two or more columns based on just that criterion then, of the
	columns that tied, the one that affects the most rows is selected.

		In practice with a well designed peeling matrix, only about sqrt(N)
	columns must be deferred to Gaussian elimination using this greedy approach.
*/

void Codec::GreedyPeeling()
{
	CAT_IF_DUMP(cout << endl << "---- GreedyPeeling ----" << endl << endl;)

	// Initialize list
	_defer_head_columns = LIST_TERM;
	_defer_count = 0;

	// Until all columns are marked,
	for (;;)
	{
		u16 best_column_i = LIST_TERM;
		u16 best_w2_refs = 0, best_row_count = 0;

		// For each column,
		PeelColumn *column = _peel_cols;
		for (u16 column_i = 0; column_i < _block_count; ++column_i, ++column)
		{
			// If column is not marked yet,
			if (column->mark == MARK_TODO)
			{
				// And if it may have the most weight-2 references
				u16 w2_refs = column->w2_refs;
				if (w2_refs >= best_w2_refs)
				{
					// Or if it has the largest row references overall,
					u16 row_count = _peel_col_refs[column_i].row_count;
					if (w2_refs > best_w2_refs || row_count >= best_row_count)
					{
						// Use that one
						best_column_i = column_i;
						best_w2_refs = w2_refs;
						best_row_count = row_count;
					}
				}
			}
		}

		// If done peeling,
		if (best_column_i == LIST_TERM)
			break;

		// Mark column as deferred
		PeelColumn *best_column = &_peel_cols[best_column_i];
		best_column->mark = MARK_DEFER;
		++_defer_count;

		// Add at head of deferred list
		best_column->next = _defer_head_columns;
		_defer_head_columns = best_column_i;

		CAT_IF_DUMP(cout << "Deferred column " << best_column_i << " for Gaussian elimination, which had " << best_column->w2_refs << " weight-2 row references" << endl;)

		// Peel resuming from where this column left off
		PeelAvalanche(best_column_i);
	}
}

/*
		After the peeling solver has completed, only Deferred columns remain
	to be solved.  Conceptually the matrix can be re-ordered in the order of
	solution so that the matrix resembles this example:

		+-----+---------+-----+
		|   1 | 1       | 721 | <-- Peeled row 1
		| 1   |   1     | 518 | <-- Peeled row 2
		| 1   | 1   1   | 934 | <-- Peeled row 3
		|   1 | 1   1 1 | 275 | <-- Peeled row 4
		+-----+---------+-----+
		| 1   | 1 1   1 | 123 | <-- Deferred rows
		|   1 |   1 1 1 | 207 | <-- Deferred rows
		+-----+---------+-----+
			^       ^       ^---- Row value (unmodified so far, ie. 1500 bytes)
			|       \------------ Peeled columns
			\-------------------- Deferred columns

		Re-ordering the actual matrix is not required, but the lower-triangular
	form of the peeled matrix is apparent in the diagram above.
*/


//// (2) Compression:

/*
	If using a naive approach, this is by far the most complex step of
	the matrix inversion.  The approach outlined here makes it minor.
	At this point the generator matrix has been re-organized
	into peeled and deferred rows and columns:

		+-----------------------+
		| p p p p p | B B | C C |
		+-----------------------+
					X
		+-----------+-----+-----+
		| 5 2 6 1 3 | 4 0 | x y |
		+-----------+-----+-----+---+   +---+
		| 1         | 0 0 | 1 0 | 0 |   | p |
		| 0 1       | 0 0 | 0 1 | 5 |   | p |
		| 1 0 1     | 1 0 | 1 0 | 3 |   | p |
		| 0 1 0 1   | 0 0 | 0 1 | 4 |   | p |
		| 0 1 0 1 1 | 1 1 | 1 0 | 1 |   | p |
		+-----------+-----+-----+---| = |---|
		| 0 1 0 1 1 | 1 1 | 0 1 | 2 |   | A |
		| 0 1 0 1 0 | 0 1 | 1 0 | 6 |   | A |
		+-----------+-----+-----+---|   |---|
		| 1 1 0 1 1 | 1 1 | 1 0 | x |   | 0 |
		| 1 0 1 1 0 | 1 0 | 0 1 | y |   | 0 |
		+-----------+-----+-----+---+   +---+

	P = Peeled rows/columns (re-ordered)
	B = Columns deferred for GE
	C = Mixing columns always deferred for GE
	A = Sparse rows from peeling matrix deferred for GE
	0 = Dense rows from check matrix deferred for GE

		Since the re-ordered matrix above is in lower triangular form,
	and since the weight of each row is limited to a constant, the cost
	of inverting the peeled matrix is O(n).  Inverting the peeled matrix
	will cause the mix and deferred columns to add up and become dense.
	These columns are stored in the same order as in the GE matrix, in
	a long vertical matrix with N rows.

		After inverting the peeling matrix, causing the mixing columns
	to become dense, the peeling matrix will be the identity matrix.
	Peeled column output blocks can be used to store the temporary blocks
	generated by this inversion.  These temporary blocks will be overwritten
	later during Substitution.  It then becomes easy to eliminate the
	peeled columns of the deferred rows.  These are then eliminated in
	O(sqrt(n)) time since there are k*sqrt(n) of them and each is sparse.

		Initially, only the dense mix and deferred columns are useful for
	forming the GE matrix.  After the GE matrix is inverted, the steps used
	to eliminate the peeled columns of the deferred rows can be followed again
	to generate the columns solved by GE in the correct output block locations.
	Note that it would be possible to generate these columns earlier but the
	output block locations are not known until GE Triangle() completes, so they
	would need to be shuffled into the right locations, which would require
	more memory and/or memcopies.
*/

void Codec::SetDeferredColumns()
{
	CAT_IF_DUMP(cout << endl << "---- SetDeferredColumns ----" << endl << endl;)

	// For each deferred column,
	PeelColumn *column;
	for (u16 ge_column_i = 0, defer_i = _defer_head_columns; defer_i != LIST_TERM; defer_i = column->next, ++ge_column_i)
	{
		column = &_peel_cols[defer_i];

		CAT_IF_DUMP(cout << "GE column " << ge_column_i << " mapped to matrix column " << defer_i << " :";)

		// Set bit for each row affected by this deferred column
		u64 *matrix_row_offset = _ge_compress_matrix + (ge_column_i >> 6);
		u64 ge_mask = (u64)1 << (ge_column_i & 63);
		PeelRefs *refs = &_peel_col_refs[defer_i];
		u16 count = refs->row_count;
		u16 *ref_row = refs->rows;
		while (count--)
		{
			u16 row_i = *ref_row++;

			CAT_IF_DUMP(cout << " " << row_i;)

			matrix_row_offset[_ge_pitch * row_i] |= ge_mask;
		}

		CAT_IF_DUMP(cout << endl;)

		// Set column map for this GE column
		_ge_col_map[ge_column_i] = defer_i;

		// Set reverse mapping also
		column->ge_column = ge_column_i;
	}

	// Set column map for each mix column
	for (u16 added_i = 0; added_i < _added_count; ++added_i)
	{
		u16 ge_column_i = _defer_count + added_i;
		u16 column_i = _block_count + added_i;

		CAT_IF_DUMP(cout << "GE column(mix) " << ge_column_i << " mapped to matrix column " << column_i << endl;)

		_ge_col_map[ge_column_i] = column_i;
	}
}

void Codec::SetMixingColumnsForDeferredRows()
{
	CAT_IF_DUMP(cout << endl << "---- SetMixingColumnsForDeferredRows ----" << endl << endl;)

	// For each deferred row,
	PeelRow *row;
	for (u16 defer_row_i = _defer_head_rows; defer_row_i != LIST_TERM; defer_row_i = row->next)
	{
		row = &_peel_rows[defer_row_i];

		CAT_IF_DUMP(cout << "Deferred row " << defer_row_i << " set mix columns :";)

		// Mark it as deferred for the following loop
		row->peel_column = LIST_TERM;

		// Generate mixing columns for this row
		u64 *ge_row = _ge_compress_matrix + _ge_pitch * defer_row_i;
		u16 a = row->mix_a;
		u16 x = row->mix_x0;

		u16 ge_column_i = _defer_count + x;
		ge_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
		CAT_IF_DUMP(cout << " " << ge_column_i;)
		IterateNextColumn(x, _added_count, _added_next_prime, a);

		ge_column_i = _defer_count + x;
		ge_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
		CAT_IF_DUMP(cout << " " << ge_column_i;)
		IterateNextColumn(x, _added_count, _added_next_prime, a);

		ge_column_i = _defer_count + x;
		ge_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
		CAT_IF_DUMP(cout << " " << ge_column_i;)

		CAT_IF_DUMP(cout << endl;)
	}
}

void Codec::PeelDiagonal()
{
	/*
		This function optimizes the block value generation by combining the first
		memcpy and memxor operations together into a three-way memxor if possible,
		using the is_copied row member.
	*/

	CAT_IF_ROWOP(int rowops = 0;)

	// For each peeled row in forward solution order,
	PeelRow *row;
	for (u16 peel_row_i = _peel_head_rows; peel_row_i != LIST_TERM; peel_row_i = row->next)
	{
		row = &_peel_rows[peel_row_i];

		// Lookup peeling results
		u16 peel_column_i = row->peel_column;
		u64 *ge_row = _ge_compress_matrix + _ge_pitch * peel_row_i;

		CAT_IF_DUMP(cout << "  Peeled row " << peel_row_i << " for peeled column " << peel_column_i << " :";)

		// Generate mixing columns for this row
		u16 a = row->mix_a;
		u16 x = row->mix_x0;

		u16 ge_column_i = _defer_count + x;
		ge_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
		CAT_IF_DUMP(cout << " " << ge_column_i;)
		IterateNextColumn(x, _added_count, _added_next_prime, a);

		ge_column_i = _defer_count + x;
		ge_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
		CAT_IF_DUMP(cout << " " << ge_column_i;)
		IterateNextColumn(x, _added_count, _added_next_prime, a);

		ge_column_i = _defer_count + x;
		ge_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
		CAT_IF_DUMP(cout << " " << ge_column_i << endl;)

		// Lookup output block
		u8 *temp_block_src = _recovery_blocks + _block_bytes * peel_column_i;

		// If row has not been copied yet,
		if (!row->is_copied)
		{
			// Copy it directly to the output symbol
			const u8 *block_src = _input_blocks + _block_bytes * peel_row_i;
			if (peel_row_i != _block_count - 1)
				memcpy(temp_block_src, block_src, _block_bytes);
			else
			{
				memcpy(temp_block_src, block_src, _input_final_bytes);
				memset(temp_block_src + _input_final_bytes, 0, _block_bytes - _input_final_bytes);
			}
			CAT_IF_ROWOP(++rowops;)

			CAT_IF_DUMP(cout << "  -- Copied from " << peel_row_i << " because has not been copied yet.  Output block = " << (int)temp_block_src[0] << endl;)

			// NOTE: Do not need to set is_copied here because no further rows reference this one
		}

		// For each row that references this one,
		PeelRefs *refs = &_peel_col_refs[peel_column_i];
		u16 count = refs->row_count;
		u16 *ref_row = refs->rows;
		while (count--)
		{
			u16 ref_row_i = *ref_row++;

			// Skip this row
			if (ref_row_i == peel_row_i) continue;

			CAT_IF_DUMP(cout << "  ++ Adding to referencing row " << ref_row_i << endl;)

			// Add GE row to referencing GE row
			u64 *ge_ref_row = _ge_compress_matrix + _ge_pitch * ref_row_i;
			for (int ii = 0; ii < _ge_pitch; ++ii) ge_ref_row[ii] ^= ge_row[ii];

			// If row is peeled,
			PeelRow *ref_row = &_peel_rows[ref_row_i];
			u16 ref_column_i = ref_row->peel_column;
			if (ref_column_i != LIST_TERM)
			{
				// Generate temporary row block value:
				u8 *temp_block_dest = _recovery_blocks + _block_bytes * ref_column_i;

				// If referencing row is already copied to the check blocks,
				if (ref_row->is_copied)
				{
					// Add this row block value to it
					memxor(temp_block_dest, temp_block_src, _block_bytes);
				}
				else
				{
					// Add this row block value with message block to it (optimization)
					const u8 *block_src = _input_blocks + _block_bytes * ref_row_i;
					if (ref_row_i != _block_count - 1)
						memxor_set(temp_block_dest, temp_block_src, block_src, _block_bytes);
					else
					{
						memxor_set(temp_block_dest, temp_block_src, block_src, _input_final_bytes);
						memcpy(temp_block_dest + _input_final_bytes, temp_block_src, _block_bytes - _input_final_bytes);
					}

					ref_row->is_copied = 1;
				}
				CAT_IF_ROWOP(++rowops;)
			} // end if referencing row is peeled
		} // next referencing row
	} // next peeled row

	CAT_IF_ROWOP(cout << "PeelDiagonal used " << rowops << " row ops" << endl;)
}

void Codec::CopyDeferredRows()
{
	CAT_IF_DUMP(cout << endl << "---- CopyDeferredRows ----" << endl << endl;)

	// For each deferred row,
	u64 *ge_row = _ge_matrix + _ge_pitch * _added_count;
	for (u16 ge_row_i = _added_count, defer_row_i = _defer_head_rows; defer_row_i != LIST_TERM;
		defer_row_i = _peel_rows[defer_row_i].next, ge_row += _ge_pitch, ++ge_row_i)
	{
		CAT_IF_DUMP(cout << "Peeled row " << defer_row_i << " for GE row " << ge_row_i << endl;)

		// Copy compress row to GE row
		u64 *compress_row = _ge_compress_matrix + _ge_pitch * defer_row_i;
		memcpy(ge_row, compress_row, _ge_pitch * sizeof(u64));

		// Set row map for this deferred row
		_ge_row_map[ge_row_i] = defer_row_i;
	}

	CAT_IF_DUMP(cout << "After copying deferred rows:" << endl;)
	CAT_IF_DUMP(PrintGEMatrix();)
}

/*
	Important Optimization: Check Matrix Structure

		After compression, the GE matrix looks like this:

		+-----------------+----------------+--------------+
		|  Dense Deferred | Dense Deferred | Dense Mixing | <- About half the matrix
		|                 |                |              |
		+-----------------+----------------+--------------+
		|                 | Dense Deferred | Dense Mixing | <- About half the matrix
		|        0        |                |              |
		|                 |----------------+--------------+
		|                 | Sparse Deferred| Dense Mixing | <- Just the last few rows
		+-----------------+----------------+--------------+
		         ^                  ^---- Middle third of the columns
				 \------ Left third of the columns

		The dense check rows are generated so that they can quickly be
	eliminated with as few row operations as possible.

		I needed to find a way to generate a binary matrix that LOOKS
	random but actually only differs by 2 bits per row.  I looked at
	using normal Gray codes or more Weyl generators but they both are
	restricted to a subset of the total possibilities.  Instead, the
	standard in-place shuffle algorithm is used to shuffle row and
	column orders to make it look random.  This new algorithm is able
	to generate nearly all possible combinations with approximately
	uniform likelihood, and the generated matrix can be addressed by
	a 32-bit seed, so it is easy to regenerate the same matrix again,
	or generate many new random matrices without reseeding.

		Shuffling generates the first row randomly, and each following
	row is XORed by two columns, one with a bit set and one without.
	The order of XOR pairs is decided by the initial shuffling.  The
	order of the generated rows is shuffled separately.

	Example output: A random 17x17 matrix

		10000001111010011
		00111110100101100
		11000001011010011
		00101100111110010
		00111100101101100
		00111100101110100
		11000011010010011
		01111110000101100
		01011111000101100
		00101000111010011
		00101100111110100
		11010011000001011
		00101000111110010
		10100000111010011
		11010111000001101
		11010111000101100
		11000011010001011

	If CAT_SHUFFLE_HALF is specified, then half way through the row is
	regenerated from scratch.  This helps improve the error correcting
	performance of the check code by adding more randomness.  When this
	is not specified, it starts becoming a problem at larger N.  For
	smaller N it doesn't affect much any may be worth turning off for
	additional performance.
*/

void Codec::MultiplyDenseRows()
{
	CAT_IF_DUMP(cout << endl << "---- MultiplyDenseRows ----" << endl << endl;)

	// Initialize PRNG
	CatsChoice prng;
	prng.Initialize(_c_seed);

	PeelColumn *column = _peel_cols;
	u16 column_i = 0;

#if !defined(CAT_LIGHT_ROWS)

	// For columns that we can put a DxD square matrix in:
	u64 *temp_row = _ge_matrix + _ge_pitch * _ge_rows;
	const int check_count = _added_count;
	u16 rows[CAT_MAX_CHECK_ROWS], bits[CAT_MAX_CHECK_ROWS];
	for (; column_i + check_count <= _block_count; column_i += check_count, column += check_count)
	{
		CAT_IF_DUMP(cout << "Shuffled check matrix starting at column " << column_i << ":" << endl;)

		// Shuffle row and bit order
		ShuffleDeck16(prng, rows, check_count);
		ShuffleDeck16(prng, bits, check_count);

		// Initialize counters
		u16 set_count = (check_count + 1) >> 1;
		u16 *set_bits = bits;
		u16 *clr_bits = set_bits + set_count;

		CAT_IF_DUMP(cout << "-- First half:" << endl;)

		// Generate first row
		memset(temp_row, 0, _ge_pitch * sizeof(u64));
		for (int ii = 0; ii < set_count; ++ii)
		{
			// If bit is peeled,
			int bit_i = set_bits[ii];
			if (column[bit_i].mark == MARK_PEEL)
			{
				// Add temp row value
				u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * column[bit_i].peel_row;
				for (int jj = 0; jj < _ge_pitch; ++jj) temp_row[jj] ^= ge_source_row[jj];
			}
			else
			{
				// Set GE bit for deferred column
				u16 ge_column_i = column[bit_i].ge_column;
				temp_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
			}
		}

		// Set up generator
		const u16 *row = rows;

		// Store first row
		u64 *ge_dest_row = _ge_matrix + _ge_pitch * *row++;
		for (int jj = 0; jj < _ge_pitch; ++jj) ge_dest_row[jj] ^= temp_row[jj];
		CAT_IF_DUMP(for (int ii = 0; ii < check_count; ++ii) cout << ((temp_row[ii >> 6] & ((u64)1 << (ii & 63))) ? '1' : '0'); cout << endl;)

		// Generate first half of rows
#if defined(CAT_SHUFFLE_HALF)
		const int loop_count = (check_count >> 1) - 1;
#else
		const int loop_count = (check_count >> 1);
#endif
		for (int ii = 0; ii < loop_count; ++ii)
		{
			int bit0 = set_bits[ii], bit1 = clr_bits[ii];

			// Add in peeled columns
			if (column[bit0].mark == MARK_PEEL)
			{
				// Add temp row value
				u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * column[bit0].peel_row;
				for (int jj = 0; jj < _ge_pitch; ++jj) temp_row[jj] ^= ge_source_row[jj];
			}
			else
			{
				// Set GE bit for deferred column
				u16 ge_column_i = column[bit0].ge_column;
				temp_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
			}

			if (column[bit1].mark == MARK_PEEL)
			{
				// Add temp row value
				u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * column[bit1].peel_row;
				for (int jj = 0; jj < _ge_pitch; ++jj) temp_row[jj] ^= ge_source_row[jj];
			}
			else
			{
				// Set GE bit for deferred column
				u16 ge_column_i = column[bit1].ge_column;
				temp_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
			}

			// Store in row
			ge_dest_row = _ge_matrix + _ge_pitch * *row++;
			for (int jj = 0; jj < _ge_pitch; ++jj) ge_dest_row[jj] ^= temp_row[jj];
			CAT_IF_DUMP(for (int ii = 0; ii < check_count; ++ii) cout << ((temp_row[ii >> 6] & ((u64)1 << (ii & 63))) ? '1' : '0'); cout << endl;)
		}

		CAT_IF_DUMP(cout << "-- Second half:" << endl;)

#if defined(CAT_SHUFFLE_HALF)
		// Shuffle bit order
		ShuffleDeck16(prng, bits, check_count);

		// Generate half-way row
		memset(temp_row, 0, _ge_pitch * sizeof(u64));
		for (int ii = 0; ii < set_count; ++ii)
		{
			// If bit is peeled,
			int bit_i = set_bits[ii];
			if (column[bit_i].mark == MARK_PEEL)
			{
				// Add temp row value
				u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * column[bit_i].peel_row;
				for (int jj = 0; jj < _ge_pitch; ++jj) temp_row[jj] ^= ge_source_row[jj];
			}
			else
			{
				// Set GE bit for deferred column
				u16 ge_column_i = column[bit_i].ge_column;
				temp_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
			}
		}

		// Store in row
		ge_dest_row = _ge_matrix + _ge_pitch * *row++;
		for (int ii = 0; ii < _ge_pitch; ++ii) ge_dest_row[ii] ^= temp_row[ii];
		CAT_IF_DUMP(for (int ii = 0; ii < check_count; ++ii) cout << ((temp_row[ii >> 6] & ((u64)1 << (ii & 63))) ? '1' : '0'); cout << endl;)

		const int second_loop_count = loop_count + (check_count & 1);
#else // CAT_SHUFFLE_HALF
		// If check count is odd,
		if (check_count & 1)
		{
			// Generate middle row
			int bit0 = set_bits[loop_count];

			// Add in peeled columns
			if (column[bit0].mark == MARK_PEEL)
			{
				// Add temp row value
				u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * column[bit0].peel_row;
				for (int jj = 0; jj < _ge_pitch; ++jj) temp_row[jj] ^= ge_source_row[jj];
			}
			else
			{
				// Set GE bit for deferred column
				u16 ge_column_i = column[bit0].ge_column;
				temp_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
			}

			// Store in row
			ge_dest_row = _ge_matrix + _ge_pitch * *row++;
			for (int jj = 0; jj < _ge_pitch; ++jj) ge_dest_row[jj] ^= temp_row[jj];
			CAT_IF_DUMP(for (int ii = 0; ii < check_count; ++ii) cout << ((temp_row[ii >> 6] & ((u64)1 << (ii & 63))) ? '1' : '0'); cout << endl;)
		}

		const int second_loop_count = loop_count - 1;
#endif // CAT_SHUFFLE_HALF

		// Generate second half of rows
		for (int ii = 0; ii < second_loop_count; ++ii)
		{
			int bit0 = set_bits[ii], bit1 = clr_bits[ii];

			// Add in peeled columns
			if (column[bit0].mark == MARK_PEEL)
			{
				// Add temp row value
				u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * column[bit0].peel_row;
				for (int jj = 0; jj < _ge_pitch; ++jj) temp_row[jj] ^= ge_source_row[jj];
			}
			else
			{
				// Set GE bit for deferred column
				u16 ge_column_i = column[bit0].ge_column;
				temp_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
			}

			if (column[bit1].mark == MARK_PEEL)
			{
				// Add temp row value
				u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * column[bit1].peel_row;
				for (int jj = 0; jj < _ge_pitch; ++jj) temp_row[jj] ^= ge_source_row[jj];
			}
			else
			{
				// Set GE bit for deferred column
				u16 ge_column_i = column[bit1].ge_column;
				temp_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
			}

			// Store in row
			ge_dest_row = _ge_matrix + _ge_pitch * *row++;
			for (int jj = 0; jj < _ge_pitch; ++jj) ge_dest_row[jj] ^= temp_row[jj];
			CAT_IF_DUMP(for (int ii = 0; ii < check_count; ++ii) cout << ((temp_row[ii >> 6] & ((u64)1 << (ii & 63))) ? '1' : '0'); cout << endl;)
		}

		CAT_IF_DUMP(cout << endl;)
	} // next column

#endif // !CAT_LIGHT_ROWS

	// For remaining columns:

	for (; column_i < _block_count; ++column_i, ++column)
	{
		// Set up for dense rows
		u32 dense_rv = prng.Next();

		// Set up for light rows
		u16 x = column_i % _light_count;
		u16 adiv = column_i / _light_count;
		u16 a = 1 + adiv % (_light_count - 1);

		// If the column is peeled,
		if (column->mark == MARK_PEEL)
		{
			// Lookup GE compress matrix source row
			u16 source_row_i = column->peel_row;
			u64 *ge_source_row = _ge_compress_matrix + _ge_pitch * source_row_i;
			u64 *ge_dest_row;

			CAT_IF_DUMP(cout << "For peeled column " << column_i << " solved by peel row " << source_row_i << " :";)

			// Light rows:
			ge_dest_row = _ge_matrix + _ge_pitch * x;
			for (int ii = 0; ii < _ge_pitch; ++ii) ge_dest_row[ii] ^= ge_source_row[ii];
			CAT_IF_DUMP(cout << " " << x;)
			IterateNextColumn(x, _light_count, _light_next_prime, a);
			ge_dest_row = _ge_matrix + _ge_pitch * x;
			for (int ii = 0; ii < _ge_pitch; ++ii) ge_dest_row[ii] ^= ge_source_row[ii];
			CAT_IF_DUMP(cout << " " << x;)
			IterateNextColumn(x, _light_count, _light_next_prime, a);
			ge_dest_row = _ge_matrix + _ge_pitch * x;
			for (int ii = 0; ii < _ge_pitch; ++ii) ge_dest_row[ii] ^= ge_source_row[ii];
			CAT_IF_DUMP(cout << " " << x << ",";)

			// Dense rows:
			ge_dest_row = _ge_matrix + _ge_pitch * _light_count;
			for (u16 dense_i = 0; dense_i < _dense_count; ++dense_i, ge_dest_row += _ge_pitch, dense_rv >>= 1)
			{
				if (dense_rv & 1)
				{
					for (int ii = 0; ii < _ge_pitch; ++ii) ge_dest_row[ii] ^= ge_source_row[ii];
					CAT_IF_DUMP(cout << " " << dense_i + _light_count;)
				}
			}
		}
		else
		{
			// Set up bit mask
			u16 ge_column_i = column->ge_column;
			u64 *ge_row = _ge_matrix + (ge_column_i >> 6);
			u64 ge_mask = (u64)1 << (ge_column_i & 63);

			CAT_IF_DUMP(cout << "For deferred column " << column_i << " at GE column " << ge_column_i << " :";)

			// Light rows:
			ge_row[_ge_pitch * x] ^= ge_mask;
			CAT_IF_DUMP(cout << " " << x;)
			IterateNextColumn(x, _light_count , _light_next_prime, a);
			ge_row[_ge_pitch * x] ^= ge_mask;
			CAT_IF_DUMP(cout << " " << x;)
			IterateNextColumn(x, _light_count , _light_next_prime, a);
			ge_row[_ge_pitch * x] ^= ge_mask;
			CAT_IF_DUMP(cout << " " << x << ",";)

			// Dense rows:
			ge_row += _ge_pitch * _light_count;
			for (u16 dense_i = 0; dense_i < _dense_count; ++dense_i, ge_row += _ge_pitch, dense_rv >>= 1)
			{
				if (dense_rv & 1)
				{
					*ge_row ^= ge_mask;
					CAT_IF_DUMP(cout << " " << dense_i + _light_count;)
				}
			}
		}
	} // next column
}


/*
	One more subtle optimization.  Depending on how the GE matrix
	is constructed, it can be put in a roughly upper-triangular form
	from the start so it looks like this:

		+---------+---------+
		| D D D D | D D D 1 |
		| D D D D | D D 1 M |
		| D D D D | D 1 M M |
		| D D D D | 1 M M M |
		+---------+---------+
		| 0 0 0 1 | M M M M |
		| 0 0 0 0 | M M M M |
		| 0 0 0 1 | M M M M |
		| 0 0 0 0 | M M M M |
		+---------+---------+

	In the example above, the top 4 rows are dense matrix rows.
	The last 4 columns are mixing columns and are also dense.
	The lower left sub-matrix is sparse and roughly upper triangular
	because it is the intersection of sparse rows in the generator
	matrix.  This form is achieved by adding deferred rows starting
	from last deferred to first, and adding deferred columns starting
	from last deferred to first.

	Gaussian elimination will proceed from left to right on the matrix.
	So, having an upper triangular form will prevent left-most zeroes
	from being eaten up when a row is eliminated by one above it.  This
	reduces row operations.  For example, GE on a 64x64 matrix will do
	on average 100 fewer row operations on this form rather than its
	transpose (where the sparse part is put in the upper right).
*/


//// (3) Gaussian Elimination

bool Codec::Triangle()
{
	CAT_IF_DUMP(cout << endl << "---- Triangle ----" << endl << endl;)

	u16 pivot_count = _defer_count + _added_count;

	// Initialize pivot array
	for (u16 pivot_i = 0; pivot_i < pivot_count; ++pivot_i)
		_ge_pivots[pivot_i] = pivot_i;

	// For each pivot to determine,
	u64 ge_mask = 1;
	for (u16 pivot_i = 0; pivot_i < pivot_count; ++pivot_i)
	{
		int word_offset = pivot_i >> 6;
		u64 *ge_matrix_offset = _ge_matrix + word_offset;

		// Find pivot
		bool found = false;
		for (u16 pivot_j = pivot_i; pivot_j < pivot_count; ++pivot_j)
		{
			// Determine if the row contains the bit we want
			u16 ge_row_j = _ge_pivots[pivot_j];
			u64 *ge_row = &ge_matrix_offset[_ge_pitch * ge_row_j];

			// If the bit was found,
			if (*ge_row & ge_mask)
			{
				found = true;
				CAT_IF_DUMP(cout << "Pivot " << pivot_i << " found on row " << ge_row_j << endl;)

				// Swap out the pivot index for this one
				u16 temp = _ge_pivots[pivot_i];
				_ge_pivots[pivot_i] = _ge_pivots[pivot_j];
				_ge_pivots[pivot_j] = temp;

				// Prepare masked first word
				u64 row0 = (*ge_row & ~(ge_mask - 1)) ^ ge_mask;

				// For each remaining unused row,
				for (u16 pivot_k = pivot_j + 1; pivot_k < pivot_count; ++pivot_k)
				{
					// Determine if the row contains the bit we want
					u16 ge_row_k = _ge_pivots[pivot_k];
					u64 *rem_row = &ge_matrix_offset[_ge_pitch * ge_row_k];

					// If the bit was found,
					if (*rem_row & ge_mask)
					{
						// Add the pivot row to eliminate the bit from this row, preserving previous bits
						*rem_row ^= row0;

						for (int ii = 1; ii < _ge_pitch - word_offset; ++ii)
							rem_row[ii] ^= ge_row[ii];
					}
				}

				break;
			}
		}

		// If pivot could not be found,
		if (!found)
		{
			_ge_resume_pivot = pivot_i;
			CAT_IF_DUMP(cout << "Inversion impossible: Pivot " << pivot_i << " of " << pivot_count << " not found!" << endl;)
			CAT_IF_ROWOP(cout << ">>>>> Inversion impossible: Pivot " << pivot_i << " of " << pivot_count << " not found!" << endl;)
			return false;
		}

		// Generate next mask
		ge_mask = CAT_ROL64(ge_mask, 1);
	}

	return true;
}

void Codec::InitializeColumnValues()
{
	CAT_IF_DUMP(cout << endl << "---- InitializeColumnValues ----" << endl << endl;)

	CAT_IF_ROWOP(u32 rowops = 0;)

	// For each pivot,
	const u16 pivot_count = _defer_count + _added_count;
	u16 pivot_i;
	for (pivot_i = 0; pivot_i < pivot_count; ++pivot_i)
	{
		u16 column_i = _ge_col_map[pivot_i];
		u16 ge_row_i = _ge_pivots[pivot_i];
		u8 *buffer_dest = _recovery_blocks + _block_bytes * column_i;

		CAT_IF_DUMP(cout << "Pivot " << pivot_i << " solving column " << column_i << " with GE row " << ge_row_i << " : ";)

		if (ge_row_i < _added_count)
		{
			memset(buffer_dest, 0, _block_bytes);

			// Store which column solves the dense row
			_ge_row_map[ge_row_i] = column_i;

			CAT_IF_DUMP(cout << "[0]";)
			CAT_IF_ROWOP(++rowops;)
		}
		else
		{
			u16 pivot_row_i = _ge_row_map[ge_row_i];
			const u8 *combo = _input_blocks + _block_bytes * pivot_row_i;

			CAT_IF_DUMP(cout << "[" << (int)combo[0] << "]";)

			// Eliminate peeled columns:
			PeelRow *row = &_peel_rows[pivot_row_i];
			u16 column_i = row->peel_x0;
			u16 a = row->peel_a;
			u16 weight = row->peel_weight;
			for (;;)
			{
				// If column is peeled,
				PeelColumn *column = &_peel_cols[column_i];
				if (column->mark == MARK_PEEL)
				{
					// If combo unused,
					if (!combo)
						memxor(buffer_dest, _recovery_blocks + _block_bytes * column_i, _block_bytes);
					else
					{
						// Use combo
						memxor_set(buffer_dest, combo, _recovery_blocks + _block_bytes * column_i, _block_bytes);
						combo = 0;
					}
					CAT_IF_ROWOP(++rowops;)
				}

				if (--weight <= 0) break;

				IterateNextColumn(column_i, _block_count, _block_next_prime, a);
			}

			// If combo still unused,
			if (combo) memcpy(buffer_dest, combo, _block_bytes);
		}
		CAT_IF_DUMP(cout << endl;)
	}

	// For each remaining pivot,
	for (; pivot_i < _ge_rows; ++pivot_i)
	{
		u16 ge_row_i = _ge_pivots[pivot_i];

		// If row is a check row,
		if (ge_row_i < _added_count)
		{
			// Mark it for skipping
			_ge_row_map[ge_row_i] = LIST_TERM;
		}
	}

	CAT_IF_ROWOP(cout << "InitializeColumnValues used " << rowops << " row ops" << endl;)
}

void Codec::AddCheckValues()
{
	CAT_IF_DUMP(cout << endl << "---- AddCheckValues ----" << endl << endl;)

	CAT_IF_ROWOP(u32 rowops = 0;)

	// Initialize PRNG
	CatsChoice prng;
	prng.Initialize(_c_seed);

	u8 *temp_block = _recovery_blocks + _block_bytes * (_block_count + _added_count);
	u16 next_dense_trigger = 0, next_dense_submatrix = 0;
	const int check_count = _added_count;

	// For columns that we can put a DxD square matrix in:

	const u8 *source_block = _recovery_blocks;
	PeelColumn *column = _peel_cols;
	u16 column_i = 0;

#if !defined(CAT_LIGHT_ROWS)

	u16 rows[CAT_MAX_CHECK_ROWS], bits[CAT_MAX_CHECK_ROWS];
	for (; column_i + check_count <= _block_count; column_i += check_count,
		column += check_count, source_block += _block_bytes * check_count)
	{
		CAT_IF_DUMP(cout << endl << "For window of columns between " << column_i << " and " << column_i + check_count - 1 << " (inclusive):" << endl;)

		// Shuffle row and bit order
		ShuffleDeck16(prng, rows, check_count);
		ShuffleDeck16(prng, bits, check_count);

		// Initialize counters
		u16 set_count = (check_count + 1) >> 1;
		u16 *set_bits = bits;
		u16 *clr_bits = set_bits + set_count;
		const u16 *row = rows;

		CAT_IF_DUMP(cout << "Generating first row " << _ge_row_map[*row] << ":";)

		// Generate first row
		const u8 *combo = 0;
		CAT_IF_ROWOP(++rowops;)
		for (int ii = 0; ii < set_count; ++ii)
		{
			// If bit is peeled,
			int bit_i = set_bits[ii];
			if (column[bit_i].mark == MARK_PEEL)
			{
				const u8 *src = source_block + _block_bytes * bit_i;

				CAT_IF_DUMP(cout << " " << column_i + bit_i;)

				// If no combo used yet,
				if (!combo)
					combo = src;
				else if (combo == temp_block)
				{
					// Else if combo has been used: XOR it in
					memxor(temp_block, src, _block_bytes);
					CAT_IF_ROWOP(++rowops;)
				}
				else
				{
					// Else if combo needs to be used: Combine into block
					memxor_set(temp_block, combo, src, _block_bytes);
					CAT_IF_ROWOP(++rowops;)
					combo = temp_block;
				}
			}
		}

		CAT_IF_DUMP(cout << endl;)

		// If no combo ever triggered,
		if (!combo)
			memset(temp_block, 0, _block_bytes);
		else
		{
			if (combo != temp_block)
			{
				// Else if never combined two: Just copy it
				memcpy(temp_block, combo, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}

			// Store first row
			u16 check_column_i = _ge_row_map[*row++];
			if (check_column_i != LIST_TERM)
			{
				memxor(_recovery_blocks + _block_bytes * check_column_i, temp_block, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}
		}

		// Generate first half of rows
#if defined(CAT_SHUFFLE_HALF)
		const int loop_count = (check_count >> 1) - 1;
#else
		const int loop_count = (check_count >> 1);
#endif
		for (int ii = 0; ii < loop_count; ++ii)
		{
			CAT_IF_DUMP(cout << "Flipping bits for derivative row " << _ge_row_map[*row] << ":";)

			int bit0 = set_bits[ii], bit1 = clr_bits[ii];

			// Add in peeled columns
			if (column[bit0].mark == MARK_PEEL)
			{
				if (column[bit1].mark == MARK_PEEL)
				{
					CAT_IF_DUMP(cout << " " << column_i + bit0 << "+" << column_i + bit1;)
					memxor_add(temp_block, source_block + _block_bytes * bit0, source_block + _block_bytes * bit1, _block_bytes);
				}
				else
				{
					CAT_IF_DUMP(cout << " " << column_i + bit0;)
					memxor(temp_block, source_block + _block_bytes * bit0, _block_bytes);
				}
				CAT_IF_ROWOP(++rowops;)
			}
			else if (column[bit1].mark == MARK_PEEL)
			{
				CAT_IF_DUMP(cout << " " << column_i + bit1;)
				memxor(temp_block, source_block + _block_bytes * bit1, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}

			CAT_IF_DUMP(cout << endl;)

			// Store in row
			u16 check_column_i = _ge_row_map[*row++];
			if (check_column_i != LIST_TERM)
			{
				memxor(_recovery_blocks + _block_bytes * check_column_i, temp_block, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}
		}

#if defined(CAT_SHUFFLE_HALF)
		// Shuffle bit order
		ShuffleDeck16(prng, bits, check_count);

		CAT_IF_DUMP(cout << "Generating middle row " << _ge_row_map[*row] << ":";)

		// Generate middle row
		combo = 0;
		CAT_IF_ROWOP(++rowops;)
		for (int ii = 0; ii < set_count; ++ii)
		{
			// If bit is peeled,
			int bit_i = set_bits[ii];
			if (column[bit_i].mark == MARK_PEEL)
			{
				const u8 *src = source_block + _block_bytes * bit_i;

				CAT_IF_DUMP(cout << " " << column_i + bit_i;)

				// If no combo used yet,
				if (!combo)
					combo = src;
				else if (combo == temp_block)
				{
					// Else if combo has been used: XOR it in
					memxor(temp_block, src, _block_bytes);
					CAT_IF_ROWOP(++rowops;)
				}
				else
				{
					// Else if combo needs to be used: Combine into block
					memxor_set(temp_block, combo, src, _block_bytes);
					CAT_IF_ROWOP(++rowops;)
					combo = temp_block;
				}
			}
		}

		CAT_IF_DUMP(cout << endl;)

		// If no combo ever triggered,
		if (!combo)
			memset(temp_block, 0, _block_bytes);
		else
		{
			if (combo != temp_block)
			{
				// Else if never combined two: Just copy it
				memcpy(temp_block, combo, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}

			// Store middle row
			u16 check_column_i = _ge_row_map[*row++];
			if (check_column_i != LIST_TERM)
			{
				memxor(_recovery_blocks + _block_bytes * check_column_i, temp_block, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}
		}

		const int second_loop_count = loop_count + (check_count & 1);
#else
		// If odd check count,
		if (check_count & 1)
		{
			int bit0 = set_bits[loop_count];

			CAT_IF_DUMP(cout << "Flipping bits for derivative row " << _ge_row_map[*row] << ":";)

			// Add in peeled columns
			if (column[bit0].mark == MARK_PEEL)
			{
				CAT_IF_DUMP(cout << " " << column_i + bit0;)
				memxor(temp_block, source_block + _block_bytes * bit0, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}

			CAT_IF_DUMP(cout << endl;)

			// Store in row
			u16 check_column_i = _ge_row_map[*row++];
			if (check_column_i != LIST_TERM)
			{
				memxor(_recovery_blocks + _block_bytes * check_column_i, temp_block, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}
		}

		const int second_loop_count = loop_count - 1;
#endif

		// Generate second half of rows
		for (int ii = 0; ii < second_loop_count; ++ii)
		{
			int bit0 = set_bits[ii], bit1 = clr_bits[ii];

			CAT_IF_DUMP(cout << "Flipping bits for derivative row " << _ge_row_map[*row] << ":";)

			// Add in peeled columns
			if (column[bit0].mark == MARK_PEEL)
			{
				if (column[bit1].mark == MARK_PEEL)
				{
					CAT_IF_DUMP(cout << " " << column_i + bit0 << "+" << column_i + bit1;)
					memxor_add(temp_block, source_block + _block_bytes * bit0, source_block + _block_bytes * bit1, _block_bytes);
				}
				else
				{
					CAT_IF_DUMP(cout << " " << column_i + bit0;)
					memxor(temp_block, source_block + _block_bytes * bit0, _block_bytes);
				}
				CAT_IF_ROWOP(++rowops;)
			}
			else if (column[bit1].mark == MARK_PEEL)
			{
				CAT_IF_DUMP(cout << " " << column_i + bit1;)
				memxor(temp_block, source_block + _block_bytes * bit1, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}

			CAT_IF_DUMP(cout << endl;)

			// Store in row
			u16 check_column_i = _ge_row_map[*row++];
			if (check_column_i != LIST_TERM)
			{
				memxor(_recovery_blocks + _block_bytes * check_column_i, temp_block, _block_bytes);
				CAT_IF_ROWOP(++rowops;)
			}
		}
	} // next column

#endif // !CAT_LIGHT_ROWS

	// For remaining columns:

	for (; column_i < _block_count; ++column_i, ++column, source_block += _block_bytes)
	{
		// Set up for dense rows
		u32 dense_rv = prng.Next();

		// Set up for light rows
		u16 x = column_i % _light_count;
		u16 adiv = column_i / _light_count;
		u16 a = 1 + adiv % (_light_count - 1);

		// If the column is peeled,
		if (column->mark == MARK_PEEL)
		{
			// Lookup GE compress matrix source row
			u16 source_row_i = column->peel_row;

			CAT_IF_DUMP(cout << "For peeled column " << column_i << " solved by peel row " << source_row_i << " :";)

			// Light rows:
			u16 dest_column_i = _ge_row_map[x];
			if (dest_column_i != LIST_TERM) memxor(_recovery_blocks + _block_bytes * dest_column_i, source_block, _block_bytes);
			CAT_IF_DUMP(cout << " " << x;)
			IterateNextColumn(x, _light_count, _light_next_prime, a);
			dest_column_i = _ge_row_map[x];
			if (dest_column_i != LIST_TERM) memxor(_recovery_blocks + _block_bytes * dest_column_i, source_block, _block_bytes);
			CAT_IF_DUMP(cout << " " << x;)
			IterateNextColumn(x, _light_count, _light_next_prime, a);
			dest_column_i = _ge_row_map[x];
			if (dest_column_i != LIST_TERM) memxor(_recovery_blocks + _block_bytes * dest_column_i, source_block, _block_bytes);
			CAT_IF_DUMP(cout << " " << x << ",";)

			// Dense rows:
			for (u16 dense_i = 0; dense_i < _dense_count; ++dense_i, dense_rv >>= 1)
			{
				if (dense_rv & 1)
				{
					dest_column_i = _ge_row_map[dense_i + _light_count];
					if (dest_column_i != LIST_TERM) memxor(_recovery_blocks + _block_bytes * dest_column_i, source_block, _block_bytes);
					CAT_IF_DUMP(cout << " " << dense_i + _light_count;)
				}
			}

			CAT_IF_DUMP(cout << endl;)
		}
	} // next column

	CAT_IF_ROWOP(cout << "AddCheckValues used " << rowops << " row ops" << endl;)
}

void Codec::AddSubdiagonalValues()
{
	CAT_IF_DUMP(cout << endl << "---- AddSubdiagonalValues ----" << endl << endl;)

	CAT_IF_ROWOP(int rowops = 0;)

	// For each pivot,
	const u16 ge_rows = _defer_count + _added_count;
	for (u16 pivot_i = 0; pivot_i < ge_rows; ++pivot_i)
	{
		u16 pivot_column_i = _ge_col_map[pivot_i];
		u16 ge_row_i = _ge_pivots[pivot_i];
		u8 *buffer_dest = _recovery_blocks + _block_bytes * pivot_column_i;

		CAT_IF_DUMP(cout << "Pivot " << pivot_i << " solving column " << pivot_column_i << "[" << (int)buffer_dest[0] << "] with GE row " << ge_row_i << " :";)

		// For each GE matrix bit in the row,
		u64 *ge_row = _ge_matrix + _ge_pitch * ge_row_i;
		u64 ge_mask = 1;
		for (u16 ge_column_i = 0; ge_column_i < pivot_i; ++ge_column_i)
		{
			// If bit is set,
			if (ge_row[ge_column_i >> 6] & ge_mask)
			{
				u16 column_i = _ge_col_map[ge_column_i];
				const u8 *peel_src = _recovery_blocks + _block_bytes * column_i;
				memxor(buffer_dest, peel_src, _block_bytes);
				CAT_IF_ROWOP(++rowops;)

				CAT_IF_DUMP(cout << " " << column_i << "=[" << (int)peel_src[0] << "]";)
			}

			ge_mask = CAT_ROL64(ge_mask, 1);
		}

		CAT_IF_DUMP(cout << endl;)
	}

	CAT_IF_ROWOP(cout << "AddSubdiagonalValues used " << rowops << " row ops" << endl;)
}


//// (4) Substitute

/*
	Windowed Back-Substitution

		The matrix to diagonalize is in upper triangular form now, where each
	row is random and dense.  Each column has a value assigned to it at this
	point but it is necessary to back-substitute up each column to eliminate
	the upper triangular part of the matrix.  Because the matrix is random,
	the optimal window size for a windowed substitution method is approx:

		w = CEIL[0.85 + 0.85*ln(r)]

		But in practice, the window size is selected from simple heuristic
	rules based on testing.  For this function, the window size stays between
	3 and 6 so it is fine to use heuristics.  The matrix may look something like:

		+---+---+---+---+
		| A | 1 | 1 | 0 |
		+---+---+---+---+
		| 0 | B | 1 | 1 |
		+---+---+---+---+
		| 0 | 0 | C | 1 |
		+---+---+---+---+
		| 0 | 0 | 0 | D |
		+---+---+---+---+

		To do the back-substitution with a window width of 2 on the above
	matrix, first back-substitute to diagonalize the lower right:

		+---+---+---+---+
		| A | 1 | 1 | 0 |
		+---+---+---+---+
		| 0 | B | 1 | 1 |
		+---+---+---+---+
		| 0 | 0 | C | 0 |
		+---+---+---+---+
		| 0 | 0 | 0 | D |
		+---+---+---+---+

		Then compute the 2-bit window:

		[0 0] = undefined
		[1 0] = C
		[0 1] = D
		[1 1] = C + D

		And substitute up the last two columns to eliminate them:

		B = B + [1 1] = B + C + D
		A = A + [1 0] = A + C

		+---+---+---+---+
		| A | 1 | 0 | 0 |
		+---+---+---+---+
		| 0 | B | 0 | 0 |
		+---+---+---+---+
		| 0 | 0 | C | 0 |
		+---+---+---+---+
		| 0 | 0 | 0 | D |
		+---+---+---+---+

		This operation is performed until the windowing method is no
	longer worthwhile, and the normal back-substitution is used on the
	remaining matrix pivots.
*/

// These are heuristic values.  Choosing better values has little effect on performance.
#define CAT_WINDOW_THRESHOLD_4 (20 + 4)
#define CAT_WINDOW_THRESHOLD_5 (40 + 5)
#define CAT_WINDOW_THRESHOLD_6 (64 + 6)
#define CAT_WINDOW_THRESHOLD_7 (128 + 7)

void Codec::BackSubstituteAboveDiagonal()
{
	CAT_IF_DUMP(cout << endl << "---- BackSubstituteAboveDiagonal ----" << endl << endl;)

	CAT_IF_ROWOP(u32 rowops = 0;)

	const int ge_rows = _defer_count + _added_count;
	int pivot_i = ge_rows - 1;

#if defined(CAT_WINDOWED_BACKSUB)
	// Build temporary storage space if windowing is to be used
	if (pivot_i >= CAT_WINDOW_THRESHOLD_5)
	{
		// Calculate initial window size
		int w, next_check_i;
		if (pivot_i >= CAT_WINDOW_THRESHOLD_7)
		{
			w = 7;
			next_check_i = CAT_WINDOW_THRESHOLD_7;
		}
		else if (pivot_i >= CAT_WINDOW_THRESHOLD_6)
		{
			w = 6;
			next_check_i = CAT_WINDOW_THRESHOLD_6;
		}
		else if (pivot_i >= CAT_WINDOW_THRESHOLD_5)
		{
			w = 5;
			next_check_i = CAT_WINDOW_THRESHOLD_5;
		}
		else
		{
			w = 4;
			next_check_i = CAT_WINDOW_THRESHOLD_4;
		}
		u32 win_lim = 1 << w;

		CAT_IF_DUMP(cout << "Activating windowed back-substitution with initial window " << w << endl;)

		// Use the first few peel column values as window table space
		// NOTE: The peeled column values were previously used up until this point,
		// but now they are unused, and so they can be reused for temporary space.
		u8 *win_table[128];
		PeelColumn *column = _peel_cols;
		u8 *column_src = _recovery_blocks;
		u32 jj = 1;
		for (u32 count = _block_count; count > 0; --count, ++column, column_src += _block_bytes)
		{
			// If column is peeled,
			if (column->mark == MARK_PEEL)
			{
				// Reuse the block value temporarily as window table space
				win_table[jj] = column_src;

				CAT_IF_DUMP(cout << "-- Window table entry " << jj << " set to column " << _block_count - count << endl;)

				// If done,
				if (++jj >= win_lim) break;
			}
		}

		CAT_IF_DUMP(if (jj < win_lim) cout << "!! Not enough space in peeled columns to generate a table.  Going back to normal back-substitute." << endl;)

		// If enough space was found,
		if (jj >= win_lim) for (;;)
		{
			// Eliminate upper triangular part above windowed bits
			u16 backsub_i = pivot_i - w + 1;

			CAT_IF_DUMP(cout << "-- Windowing from " << backsub_i << " to " << pivot_i << " (inclusive)" << endl;)

			// For each column,
			u64 ge_mask = (u64)1 << (pivot_i & 63);
			for (int src_pivot_i = pivot_i; src_pivot_i > backsub_i; --src_pivot_i)
			{
				// Set up for iteration
				const u64 *ge_row = _ge_matrix + (src_pivot_i >> 6);
				const u8 *src = _recovery_blocks + _block_bytes * _ge_col_map[src_pivot_i];

				CAT_IF_DUMP(cout << "Back-substituting small triangle from pivot " << src_pivot_i << "[" << (int)src[0] << "] :";)

				// For each upper triangular bit,
				for (int dest_pivot_i = backsub_i; dest_pivot_i < src_pivot_i; ++dest_pivot_i)
				{
					// If bit is set,
					if (ge_row[_ge_pitch * _ge_pivots[dest_pivot_i]] & ge_mask)
					{
						CAT_IF_DUMP(cout << " " << dest_pivot_i;)

						// Back-substitute
						// NOTE: Because the values exist on the diagonal, the row is also the column index
						memxor(_recovery_blocks + _block_bytes * _ge_col_map[dest_pivot_i], src, _block_bytes);
						CAT_IF_ROWOP(++rowops;)
					}
				}

				CAT_IF_DUMP(cout << endl;)

				// Generate next mask
				ge_mask = CAT_ROR64(ge_mask, 1);
			}

			CAT_IF_DUMP(cout << "-- Generating window table with " << w << " bits" << endl;)

			// Generate window table: 2 bits
			win_table[1] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i];
			win_table[2] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 1];
			memxor_set(win_table[3], win_table[1], win_table[2], _block_bytes);
			CAT_IF_ROWOP(++rowops;)

			// Generate window table: 3 bits
			win_table[4] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 2];
			memxor_set(win_table[5], win_table[1], win_table[4], _block_bytes);
			memxor_set(win_table[6], win_table[2], win_table[4], _block_bytes);
			memxor_set(win_table[7], win_table[1], win_table[6], _block_bytes);
			CAT_IF_ROWOP(rowops += 3;)

			// Generate window table: 4 bits
			win_table[8] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 3];
			for (int ii = 1; ii < 8; ++ii)
				memxor_set(win_table[8 + ii], win_table[ii], win_table[8], _block_bytes);
			CAT_IF_ROWOP(rowops += 7;)

			// Generate window table: 5+ bits
			if (w >= 5)
			{
				win_table[16] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 4];
				for (int ii = 1; ii < 16; ++ii)
					memxor_set(win_table[16 + ii], win_table[ii], win_table[16], _block_bytes);
				CAT_IF_ROWOP(rowops += 15;)

				if (w >= 6)
				{
					win_table[32] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 5];
					for (int ii = 1; ii < 32; ++ii)
						memxor_set(win_table[32 + ii], win_table[ii], win_table[32], _block_bytes);
					CAT_IF_ROWOP(rowops += 31;)

					if (w >= 7)
					{
						win_table[64] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 6];
						for (int ii = 1; ii < 64; ++ii)
							memxor_set(win_table[64 + ii], win_table[ii], win_table[64], _block_bytes);
						CAT_IF_ROWOP(rowops += 63;)
					}
				}
			}

			// If not straddling words,
			u32 first_word = backsub_i >> 6;
			u32 shift0 = backsub_i & 63;
			u32 last_word = pivot_i >> 6;
			if (first_word == last_word)
			{
				// For each pivot row,
				for (u16 above_pivot_i = 0; above_pivot_i < backsub_i; ++above_pivot_i)
				{
					// Calculate window bits
					u64 *ge_row = _ge_matrix + first_word + _ge_pitch * _ge_pivots[above_pivot_i];
					u32 win_bits = (u32)(ge_row[0] >> shift0) & (win_lim - 1);

					// If any XOR needs to be performed,
					if (win_bits != 0)
					{
						CAT_IF_DUMP(cout << "Adding window table " << win_bits << " to pivot " << above_pivot_i << endl;)

						// Back-substitute
						memxor(_recovery_blocks + _block_bytes * _ge_col_map[above_pivot_i], win_table[win_bits], _block_bytes);
						CAT_IF_ROWOP(++rowops;)
					}
				}
			}
			else // Rare: Straddling case
			{
				u32 shift1 = 64 - shift0;

				// For each pivot row,
				for (u16 above_pivot_i = 0; above_pivot_i < backsub_i; ++above_pivot_i)
				{
					// Calculate window bits
					u64 *ge_row = _ge_matrix + first_word + _ge_pitch * _ge_pivots[above_pivot_i];
					u32 win_bits = ( (u32)(ge_row[0] >> shift0) | (u32)(ge_row[1] << shift1) ) & (win_lim - 1);

					// If any XOR needs to be performed,
					if (win_bits != 0)
					{
						CAT_IF_DUMP(cout << "Adding window table " << win_bits << " to pivot " << above_pivot_i << endl;)

						// Back-substitute
						memxor(_recovery_blocks + _block_bytes * _ge_col_map[above_pivot_i], win_table[win_bits], _block_bytes);
						CAT_IF_ROWOP(++rowops;)
					}
				}
			}

			// Calculate next window size
			pivot_i -= w;
			if (pivot_i < next_check_i)
			{
				if (pivot_i >= CAT_WINDOW_THRESHOLD_6)
				{
					w = 6;
					next_check_i = CAT_WINDOW_THRESHOLD_6;
				}
				else if (pivot_i >= CAT_WINDOW_THRESHOLD_5)
				{
					w = 5;
					next_check_i = CAT_WINDOW_THRESHOLD_5;
				}
				else if (pivot_i >= CAT_WINDOW_THRESHOLD_4)
				{
					w = 4;
					next_check_i = CAT_WINDOW_THRESHOLD_4;
				}
				else break;

				// Update window limit
				win_lim = 1 << w;
			}
		}
	}
#endif // CAT_WINDOWED_BACKSUB

	// For each remaining pivot,
	u64 ge_mask = (u64)1 << (pivot_i & 63);
	for (; pivot_i >= 0; --pivot_i)
	{
		// Calculate source
		const u8 *src = _recovery_blocks + _block_bytes * _ge_col_map[pivot_i];

		CAT_IF_DUMP(cout << "Pivot " << pivot_i << "[" << (int)src[0] << "]:";)

		// For each pivot row above it,
		u64 *ge_row = _ge_matrix + (pivot_i >> 6);
		for (int above_i = 0; above_i < pivot_i; ++above_i)
		{
			// If bit is set in that row,
			if (ge_row[_ge_pitch * _ge_pivots[above_i]] & ge_mask)
			{
				// Back-substitute
				memxor(_recovery_blocks + _block_bytes * _ge_col_map[above_i], src, _block_bytes);
				CAT_IF_ROWOP(++rowops;)

				CAT_IF_DUMP(cout << " " << above_i;)
			}
		}

		CAT_IF_DUMP(cout << endl;)

		// Generate next mask
		ge_mask = CAT_ROR64(ge_mask, 1);
	}

	CAT_IF_ROWOP(cout << "BackSubstituteAboveDiagonal used " << rowops << " row ops" << endl;)
}

void Codec::Substitute()
{
	CAT_IF_DUMP(cout << endl << "---- Substitute ----" << endl << endl;)

	CAT_IF_ROWOP(u32 rowops = 0;)

	// For each column that has been peeled,
	u16 ge_rows = _defer_count + _added_count;
	PeelRow *row;
	for (u16 row_i = _peel_head_rows; row_i != LIST_TERM; row_i = row->next)
	{
		row = &_peel_rows[row_i];
		u16 dest_column_i = row->peel_column;
		u8 *dest = _recovery_blocks + _block_bytes * dest_column_i;

		CAT_IF_DUMP(cout << "Generating column " << dest_column_i << ":";)

		const u8 *input_src = _input_blocks + _block_bytes * row_i;
		CAT_IF_DUMP(cout << " " << row_i << ":[" << (int)input_src[0] << "]";)

		// Set up mixing column generator
		u16 mix_a = row->mix_a;
		u16 mix_x = row->mix_x0;
		const u8 *src = _recovery_blocks + _block_bytes * (_block_count + mix_x);

		// If copying from final block,
		if (row_i != _block_count - 1)
			memxor_set(dest, src, input_src, _block_bytes);
		else
		{
			memxor_set(dest, src, input_src, _input_final_bytes);
			memcpy(dest + _input_final_bytes, src, _block_bytes - _input_final_bytes);
		}
		CAT_IF_ROWOP(++rowops;)

		// Add next two mixing columns in
		IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
		const u8 *src0 = _recovery_blocks + _block_bytes * (_block_count + mix_x);
		IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
		const u8 *src1 = _recovery_blocks + _block_bytes * (_block_count + mix_x);
		memxor_add(dest, src0, src1, _block_bytes);
		CAT_IF_ROWOP(++rowops;)

		// If at least two peeling columns are set,
		u16 weight = row->peel_weight;
		if (weight >= 2) // common case:
		{
			u16 a = row->peel_a;
			u16 column0 = row->peel_x0;
			--weight;

			u16 column_i = column0;
			IterateNextColumn(column_i, _block_count, _block_next_prime, a);

			// Common case:
			if (column0 != dest_column_i)
			{
				const u8 *peel0 = _recovery_blocks + _block_bytes * column0;

				// Common case:
				if (column_i != dest_column_i)
					memxor_add(dest, peel0, _recovery_blocks + _block_bytes * column_i, _block_bytes);
				else // rare:
					memxor(dest, peel0, _block_bytes);
			}
			else // rare:
				memxor(dest, _recovery_blocks + _block_bytes * column_i, _block_bytes);
			CAT_IF_ROWOP(++rowops;)

			// For each remaining column,
			while (--weight > 0)
			{
				IterateNextColumn(column_i, _block_count, _block_next_prime, a);
				const u8 *src = _recovery_blocks + _block_bytes * column_i;

				CAT_IF_DUMP(cout << " " << column_i;)

				// If column is not the solved one,
				if (column_i != dest_column_i)
				{
					memxor(dest, src, _block_bytes);
					CAT_IF_ROWOP(++rowops;)
					CAT_IF_DUMP(cout << "[" << (int)src[0] << "]";)
				}
				else
				{
					CAT_IF_DUMP(cout << "*";)
				}
			}
		} // end if weight 2

		CAT_IF_DUMP(cout << endl;)
	}

	CAT_IF_ROWOP(cout << "Substitute used " << rowops << " row ops" << endl;)
}


//// Compression-based Substitute

/*
		Instead of throwing away the compression matrix, this approach reuses
	the temporary values calculated during compression, and eliminates the
	dense compression matrix with windowed back-substitution.  It is only best
	to use this approach for smaller N, because it scales as O(N^^1.4).  The
	reason why you would want to use it at all is because it can reuse the
	work from compression which had diagonalized the peeling matrix.

		The thing is that in practice the codec is already so fast that the
	added complexity kills the advantage of reducing the row operation count.
	This might become useful at some point if I find a way to simplify the
	inner loops.  Especially because it needs to allocate more memory for
	temporary space it is pretty unlikely it will ever be better.  This is
	tested and working though, so I'm leaving it in just in case.
*/

#if defined(CAT_REUSE_COMPRESS)

#define CAT_DISCARD_COMPRESS_MIN 32
#define CAT_DISCARD_COMPRESS_MAX 1024

#define CAT_COMP_WINDOW_THRESHOLD_7 512

void Codec::CompressionBasedSubstitute()
{
	CAT_IF_DUMP(cout << endl << "---- CompressionBasedSubstitute ----" << endl << endl;)

	CAT_IF_ROWOP(u32 window_rowops = 0;)

	const int ge_rows = _defer_count + _added_count;
	int pivot_i = ge_rows - 1;

	// Calculate initial window size
	int w;
	if (_block_count >= CAT_COMP_WINDOW_THRESHOLD_7)
		w = 7;
	else
		w = 6;
	const u32 win_lim = 1 << w;

	CAT_IF_DUMP(cout << "Activating windowed back-substitution with window " << w << endl;)

	// Allocate memory for window table
	u8 *win_table[128];
	int win_table_bytes = _block_bytes * win_lim;
	_win_table_data = new u8[win_table_bytes];
	if (_win_table_data)
	{
		// Initialize window table
		u8 *ptr = _win_table_data;
		for (u32 ii = 0; ii < win_lim; ++ii, ptr += _block_bytes)
			win_table[ii] = ptr;
	}

	if (_win_table_data) do
	{
		// Eliminate upper triangular part above windowed bits
		u16 backsub_i = pivot_i - w + 1;

		CAT_IF_DUMP(cout << "-- Windowing from " << backsub_i << " to " << pivot_i << " (inclusive)" << endl;)

		// For each column,
		u64 ge_mask = (u64)1 << (pivot_i & 63);
		for (int src_pivot_i = pivot_i; src_pivot_i > backsub_i; --src_pivot_i)
		{
			// Set up for iteration
			const u64 *ge_row = _ge_matrix + (src_pivot_i >> 6);
			const u8 *src = _recovery_blocks + _block_bytes * _ge_col_map[src_pivot_i];

			CAT_IF_DUMP(cout << "Back-substituting small triangle from pivot " << src_pivot_i << "[" << (int)src[0] << "] :";)

			// For each upper triangular bit,
			for (int dest_pivot_i = backsub_i; dest_pivot_i < src_pivot_i; ++dest_pivot_i)
			{
				// If bit is set,
				if (ge_row[_ge_pitch * _ge_pivots[dest_pivot_i]] & ge_mask)
				{
					CAT_IF_DUMP(cout << " " << dest_pivot_i;)

					// Back-substitute
					// NOTE: Because the values exist on the diagonal, the row is also the column index
					memxor(_recovery_blocks + _block_bytes * _ge_col_map[dest_pivot_i], src, _block_bytes);
					CAT_IF_ROWOP(++window_rowops;)
				}
			}

			CAT_IF_DUMP(cout << endl;)

			// Generate next mask
			ge_mask = CAT_ROR64(ge_mask, 1);
		}

		CAT_IF_DUMP(cout << "-- Generating window table with " << w << " bits" << endl;)

		// Generate window table: 2 bits
		win_table[1] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i];
		win_table[2] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 1];
		memxor_set(win_table[3], win_table[1], win_table[2], _block_bytes);
		CAT_IF_ROWOP(++window_rowops;)

		// Generate window table: 3 bits
		win_table[4] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 2];
		memxor_set(win_table[5], win_table[1], win_table[4], _block_bytes);
		memxor_set(win_table[6], win_table[2], win_table[4], _block_bytes);
		memxor_set(win_table[7], win_table[1], win_table[6], _block_bytes);
		CAT_IF_ROWOP(window_rowops += 3;)

		// Generate window table: 4 bits
		win_table[8] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 3];
		for (int ii = 1; ii < 8; ++ii)
			memxor_set(win_table[8 + ii], win_table[ii], win_table[8], _block_bytes);
		CAT_IF_ROWOP(window_rowops += 7;)

		// Generate window table: 5 bits
		win_table[16] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 4];
		for (int ii = 1; ii < 16; ++ii)
			memxor_set(win_table[16 + ii], win_table[ii], win_table[16], _block_bytes);
		CAT_IF_ROWOP(window_rowops += 15;)

		// Generate window table: 6 bits
		win_table[32] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 5];
		for (int ii = 1; ii < 32; ++ii)
			memxor_set(win_table[32 + ii], win_table[ii], win_table[32], _block_bytes);
		CAT_IF_ROWOP(window_rowops += 31;)

		// Generate window table: 7+ bits
		if (w >= 7)
		{
			win_table[64] = _recovery_blocks + _block_bytes * _ge_col_map[backsub_i + 6];
			for (int ii = 1; ii < 64; ++ii)
				memxor_set(win_table[64 + ii], win_table[ii], win_table[64], _block_bytes);
			CAT_IF_ROWOP(window_rowops += 63;)
		}

		// If not straddling words,
		u32 first_word = backsub_i >> 6;
		u32 shift0 = backsub_i & 63;
		u32 last_word = pivot_i >> 6;
		int flip_count = 0;
		if (first_word == last_word)
		{
			// For each pivot row,
			for (u16 above_pivot_i = 0; above_pivot_i < backsub_i; ++above_pivot_i)
			{
				// Calculate window bits
				u64 *ge_row = _ge_matrix + first_word + _ge_pitch * _ge_pivots[above_pivot_i];
				u32 win_bits = (u32)(ge_row[0] >> shift0) & (win_lim - 1);

				// If any XOR needs to be performed,
				if (win_bits != 0)
				{
					CAT_IF_DUMP(cout << "Adding window table " << win_bits << " to pivot " << above_pivot_i << endl;)

					// Back-substitute
					memxor(_recovery_blocks + _block_bytes * _ge_col_map[above_pivot_i], win_table[win_bits], _block_bytes);
					CAT_IF_ROWOP(++window_rowops;)
				}
			}

			// For each row of the compression matrix,
			PeelRow *row = _peel_rows;
			u64 *ge_row = _ge_compress_matrix + first_word;
			for (u16 row_i = 0; row_i < _block_count; ++row_i, ge_row += _ge_pitch, ++row)
			{
				// If row is not peeled,
				if (row->peel_column == LIST_TERM)
					continue;

				// Calculate window bits
				u32 win_bits = (u32)(ge_row[0] >> shift0) & (win_lim - 1);

				// If any XOR needs to be performed,
				if (win_bits != 0)
				{
					CAT_IF_DUMP(cout << "Adding window table " << win_bits << " to peel column " << row->peel_column << endl;)

					// Back-substitute
					memxor(_recovery_blocks + _block_bytes * row->peel_column, win_table[win_bits], _block_bytes);
					CAT_IF_ROWOP(++window_rowops;)

					++flip_count;
				}
			}
		}
		else // Rare: Straddling case
		{
			u32 shift1 = 64 - shift0;

			// For each pivot row,
			for (u16 above_pivot_i = 0; above_pivot_i < backsub_i; ++above_pivot_i)
			{
				// Calculate window bits
				u64 *ge_row = _ge_matrix + first_word + _ge_pitch * _ge_pivots[above_pivot_i];
				u32 win_bits = ( (u32)(ge_row[0] >> shift0) | (u32)(ge_row[1] << shift1) ) & (win_lim - 1);

				// If any XOR needs to be performed,
				if (win_bits != 0)
				{
					CAT_IF_DUMP(cout << "Adding window table " << win_bits << " to pivot " << above_pivot_i << endl;)

					// Back-substitute
					memxor(_recovery_blocks + _block_bytes * _ge_col_map[above_pivot_i], win_table[win_bits], _block_bytes);
					CAT_IF_ROWOP(++window_rowops;)
				}
			}

			// For each row of the compression matrix,
			PeelRow *row = _peel_rows;
			u64 *ge_row = _ge_compress_matrix + first_word;
			for (u16 row_i = 0; row_i < _block_count; ++row_i, ge_row += _ge_pitch, ++row)
			{
				// If row is not peeled,
				if (row->peel_column == LIST_TERM)
					continue;

				// Calculate window bits
				u32 win_bits = ( (u32)(ge_row[0] >> shift0) | (u32)(ge_row[1] << shift1) ) & (win_lim - 1);

				// If any XOR needs to be performed,
				if (win_bits != 0)
				{
					CAT_IF_DUMP(cout << "Adding window table " << win_bits << " to peel column " << row->peel_column << endl;)

					// Back-substitute
					memxor(_recovery_blocks + _block_bytes * row->peel_column, win_table[win_bits], _block_bytes);
					CAT_IF_ROWOP(++window_rowops;)

					++flip_count;
				}
			}
		} // end if straddle

		// Calculate next window size
		pivot_i -= w;

		// Stop using window optimization if matrix is zeroish
		if (flip_count < win_lim / 2) break;

	} while (pivot_i > w);

	CAT_IF_ROWOP(int remain_rowops = 0;)

	// For each remaining pivot,
	int final_pivot_i = pivot_i;
	u64 ge_mask = (u64)1 << (pivot_i & 63);
	for (; pivot_i >= 0; --pivot_i)
	{
		// Calculate source
		const u8 *src = _recovery_blocks + _block_bytes * _ge_col_map[pivot_i];

		CAT_IF_DUMP(cout << "Pivot " << pivot_i << "[" << (int)src[0] << "]:";)

		// For each pivot row above it,
		u64 *ge_row = _ge_matrix + (pivot_i >> 6);
		for (int above_i = 0; above_i < pivot_i; ++above_i)
		{
			// If bit is set in that row,
			if (ge_row[_ge_pitch * _ge_pivots[above_i]] & ge_mask)
			{
				// Back-substitute
				memxor(_recovery_blocks + _block_bytes * _ge_col_map[above_i], src, _block_bytes);
				CAT_IF_ROWOP(++remain_rowops;)

				CAT_IF_DUMP(cout << " " << above_i;)
			}
		}

		CAT_IF_DUMP(cout << endl;)

		// Generate next mask
		ge_mask = CAT_ROR64(ge_mask, 1);
	}

	// For each row of the compression matrix,
	PeelRow *row = _peel_rows;
	u64 *ge_row = _ge_compress_matrix;
	for (u16 row_i = 0; row_i < _block_count; ++row_i, ge_row += _ge_pitch, ++row)
	{
		// If row is not peeled,
		if (row->peel_column == LIST_TERM)
			continue;

		// Lookup peeling results
		u16 peel_column_i = row->peel_column;
		u8 *dest = _recovery_blocks + _block_bytes * peel_column_i;

		// For each column,
		u64 ge_mask = 1;
		for (u16 ge_column_i = 0; ge_column_i <= final_pivot_i; ++ge_column_i)
		{
			// If bit is set,
			if (ge_row[ge_column_i >> 6] & ge_mask)
			{
				memxor(dest, _recovery_blocks + _block_bytes * _ge_col_map[ge_column_i], _block_bytes);
				CAT_IF_ROWOP(++remain_rowops;)
			}

			// Generate next mask
			ge_mask = CAT_ROL64(ge_mask, 1);
		}
	}

	CAT_IF_ROWOP(cout << "CompressionBasedSubstitute used " << window_rowops << " + " << remain_rowops << " = " << window_rowops + remain_rowops << " row ops" << endl;)
}

#endif // CAT_REUSE_COMPRESS


//// Main Driver

Result Codec::ChooseMatrix(int message_bytes, int block_bytes)
{
	CAT_IF_DUMP(cout << endl << "---- ChooseMatrix ----" << endl << endl;)

	// Calculate message block count
	_block_bytes = block_bytes;
	_block_count = (message_bytes + _block_bytes - 1) / _block_bytes;
	_block_next_prime = NextPrime16(_block_count);

	CAT_IF_DUMP(cout << "Total message = " << message_bytes << " bytes.  Block bytes = " << _block_bytes << endl;)
	CAT_IF_DUMP(cout << "Block count = " << _block_count << " +Prime=" << _block_next_prime << endl;)

	// Lookup generator matrix parameters
	if (!GenerateMatrixParameters(_block_count, _p_seed, _c_seed, _light_count, _dense_count))
		return R_BAD_INPUT;

	_light_next_prime = NextPrime16(_light_count);
	_added_count = _light_count + _dense_count;
	_added_next_prime = NextPrime16(_added_count);

	CAT_IF_DUMP(cout << "Peel seed = " << _p_seed << "  Check seed = " << _c_seed << endl;)
	CAT_IF_DUMP(cout << "Light count = " << _light_count << " +Prime=" << _light_next_prime << endl;)
	CAT_IF_DUMP(cout << " + Dense count = " << _dense_count << " = Total added count = " << _added_count << " +Prime=" << _added_next_prime << endl;)

	// Initialize lists
	_peel_head_rows = LIST_TERM;
	_peel_tail_rows = 0;
	_defer_head_rows = LIST_TERM;

	return R_WIN;
}

Result Codec::SolveMatrix()
{
	// (1) Peeling

	GreedyPeeling();

	CAT_IF_DUMP( PrintPeeled(); )
	CAT_IF_DUMP( PrintDeferredRows(); )
	CAT_IF_DUMP( PrintDeferredColumns(); )

	// (2) Compression

	if (!AllocateMatrix())
		return R_OUT_OF_MEMORY;

	CAT_IF_DUMP( PrintGEMatrix(); )
	CAT_IF_DUMP( PrintCompressMatrix(); )

	SetDeferredColumns();
	SetMixingColumnsForDeferredRows();
	PeelDiagonal();
	CopyDeferredRows();
	MultiplyDenseRows();
	AddInvertibleGF2Matrix(_ge_matrix, _defer_count, _ge_pitch, _added_count);

#if defined(CAT_DUMP_CODEC_DEBUG) || defined(CAT_DUMP_GE_MATRIX)
	cout << "After Compress:" << endl;
	PrintGEMatrix();
#endif
	CAT_IF_DUMP( PrintCompressMatrix(); )

	// (3) Gaussian Elimination

	if (!Triangle())
	{
		CAT_IF_DUMP( cout << "After Triangle FAILED:" << endl; )
		CAT_IF_DUMP( PrintGEMatrix(); )

		return R_MORE_BLOCKS;
	}

#if defined(CAT_DUMP_CODEC_DEBUG) || defined(CAT_DUMP_GE_MATRIX)
	cout << "After Triangle:" << endl;
	PrintGEMatrix();
#endif

	return R_WIN;
}

void Codec::GenerateRecoveryBlocks()
{
	// (4) Substitution

	InitializeColumnValues();
	AddCheckValues();
	AddSubdiagonalValues();

#if defined(CAT_REUSE_COMPRESS)

	// If block count is within re-use range for compression matrix,
	if (_block_count >= CAT_DISCARD_COMPRESS_MIN && _block_count <= CAT_DISCARD_COMPRESS_MAX)
	{
		// Reuse the compression matrix to speed up substitution
		CompressionBasedSubstitute();
	}
	else
#endif // CAT_REUSE_COMPRESS
	{
		BackSubstituteAboveDiagonal();
		Substitute();
	}
}

bool Codec::ResumeSolveMatrix(u32 id, const void *block)
{
	CAT_IF_DUMP(cout << endl << "---- ResumeSolveMatrix ----" << endl << endl;)

	// If there is no room for it,
	u16 row_i, ge_row_i;
	if (_used_count >= _block_count + _extra_count)
	{
		// Find a non-check row to reuse
		for (u16 pivot_i = _ge_resume_pivot; pivot_i < _ge_rows; ++pivot_i)
		{
			ge_row_i = _ge_pivots[pivot_i];
			if (ge_row_i >= _added_count) break;
		}

		row_i = _ge_row_map[ge_row_i];
	}
	else
	{
		// Look up storage space
		ge_row_i = _ge_rows++;
		row_i = _used_count++;
		_ge_row_map[ge_row_i] = row_i;
	}

	CAT_IF_DUMP(cout << "Resuming using row slot " << row_i << " and GE row " << ge_row_i << endl;)

	// Update row data needed at this point
	PeelRow *row = &_peel_rows[row_i];
	row->id = id;

	// Copy new block to input blocks
	u8 *block_store_dest = _input_blocks + _block_bytes * row_i;
	memcpy(block_store_dest, block, _block_bytes);

	// Generate new GE row
	u64 *ge_new_row = _ge_matrix + _ge_pitch * ge_row_i;
	memset(ge_new_row, 0, _ge_pitch * sizeof(u64));

	u16 peel_weight, peel_a, peel_x, mix_a, mix_x;
	GeneratePeelRow(id, _p_seed, _block_count, _added_count,
		peel_weight, peel_a, peel_x, mix_a, mix_x);

	// Store row parameters
	row->peel_weight = peel_weight;
	row->peel_a = peel_a;
	row->peel_x0 = peel_x;
	row->mix_a = mix_a;
	row->mix_x0 = mix_x;

	// Generate mixing bits in GE row
	u16 ge_column_i = mix_x + _defer_count;
	ge_new_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
	IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
	ge_column_i = mix_x + _defer_count;
	ge_new_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
	IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
	ge_column_i = mix_x + _defer_count;
	ge_new_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);

	// Generate peeled bits in GE row
	for (;;)
	{
		PeelColumn *ref_col = &_peel_cols[peel_x];

		// If column is peeled,
		if (ref_col->mark == MARK_PEEL)
		{
			// Add compress row to the new GE row
			u16 row_i = ref_col->peel_row;
			const u64 *ge_src_row = _ge_compress_matrix + _ge_pitch * row_i;
			for (int ii = 0; ii < _ge_pitch; ++ii) ge_new_row[ii] ^= ge_src_row[ii];
		}
		else
		{
			// Set bit for this deferred column
			u16 ge_column_i = ref_col->ge_column;
			ge_new_row[ge_column_i >> 6] ^= (u64)1 << (ge_column_i & 63);
		}

		if (--peel_weight <= 0) break;

		IterateNextColumn(peel_x, _block_count, _block_next_prime, peel_a);
	}

	// Flip GE row bits up to the pivot resume point
	u16 pivot_i = _ge_resume_pivot;
	u64 ge_mask = 1;
	for (u16 pivot_j = 0; pivot_j < pivot_i; ++pivot_j)
	{
		int word_offset = pivot_j >> 6;
		u64 *rem_row = &ge_new_row[word_offset];

		// If bit is set,
		if (*rem_row & ge_mask)
		{
			u16 ge_pivot_j = _ge_pivots[pivot_j];
			u64 *ge_pivot_row = _ge_matrix + word_offset + _ge_pitch * ge_pivot_j;

			u64 row0 = (*ge_pivot_row & ~(ge_mask - 1)) ^ ge_mask;
			*rem_row ^= row0;

			for (int ii = 1; ii < _ge_pitch - word_offset; ++ii)
				rem_row[ii] ^= ge_pivot_row[ii];
		}

		// Generate next mask
		ge_mask = CAT_ROL64(ge_mask, 1);
	}

	// If the next pivot was not found on this row,
	if ((ge_new_row[pivot_i >> 6] & ge_mask) == 0)
	{
		// Initialize new pivot element
		_ge_pivots[ge_row_i] = ge_row_i;

		return false; // Maybe next time...
	}

	// Swap out the pivot index for this one
	_ge_pivots[ge_row_i] = _ge_pivots[pivot_i];
	_ge_pivots[pivot_i] = ge_row_i;

	// NOTE: Pivot was found and is definitely not set anywhere else
	// so it doesn't need to be cleared from any other GE rows.

	// For each pivot to determine,
	const u16 pivot_count = _defer_count + _added_count;
	ge_mask = CAT_ROL64(ge_mask, 1);
	for (++pivot_i; pivot_i < pivot_count; ++pivot_i)
	{
		int word_offset = pivot_i >> 6;
		u64 *ge_matrix_offset = _ge_matrix + word_offset;

		// Find pivot
		bool found = false;
		for (u16 pivot_j = pivot_i; pivot_j < _ge_rows; ++pivot_j)
		{
			// Determine if the row contains the bit we want
			u16 ge_row_j = _ge_pivots[pivot_j];
			u64 *ge_row = &ge_matrix_offset[_ge_pitch * ge_row_j];

			// If the bit was found,
			if (*ge_row & ge_mask)
			{
				found = true;
				CAT_IF_DUMP(cout << "Pivot " << pivot_i << " found on row " << ge_row_j << endl;)

				// Swap out the pivot index for this one
				u16 temp = _ge_pivots[pivot_i];
				_ge_pivots[pivot_i] = _ge_pivots[pivot_j];
				_ge_pivots[pivot_j] = temp;

				// Prepare masked first word
				u64 row0 = (*ge_row & ~(ge_mask - 1)) ^ ge_mask;

				// For each remaining unused row,
				for (u16 pivot_k = pivot_j + 1; pivot_k < _ge_rows; ++pivot_k)
				{
					// Determine if the row contains the bit we want
					u16 ge_row_k = _ge_pivots[pivot_k];
					u64 *rem_row = &ge_matrix_offset[_ge_pitch * ge_row_k];

					// If the bit was found,
					if (*rem_row & ge_mask)
					{
						// Add the pivot row to eliminate the bit from this row, preserving previous bits
						*rem_row ^= row0;

						for (int ii = 1; ii < _ge_pitch - word_offset; ++ii)
							rem_row[ii] ^= ge_row[ii];
					}
				}

				break;
			}
		}

		// If pivot could not be found,
		if (!found)
		{
			_ge_resume_pivot = pivot_i;
			CAT_IF_DUMP(cout << "Inversion impossible: Pivot " << pivot_i << " of " << pivot_count << " not found!" << endl;)
			CAT_IF_ROWOP(cout << ">>>>> Inversion impossible: Pivot " << pivot_i << " of " << pivot_count << " not found!" << endl;)
			return false;
		}

		// Generate next mask
		ge_mask = CAT_ROL64(ge_mask, 1);
	}

	return true;
}

void Codec::ReconstructOutput(void *message_out)
{
	CAT_IF_DUMP(cout << endl << "---- ReconstructOutput ----" << endl << endl;)

	u8 *output_blocks = reinterpret_cast<u8 *>( message_out );

#if defined(CAT_COPY_FIRST_N)
	// Re-purpose and initialize an array to store whether or not each row id needs to be regenerated
	u8 *copied_rows = reinterpret_cast<u8*>( _peel_cols );
	memset(copied_rows, 0, _block_count);

	// Copy any original message rows that were received:
	// For each row,
	PeelRow *row = _peel_rows;
	const u8 *src = _input_blocks;
	for (u16 row_i = 0; row_i < _used_count; ++row_i, ++row, src += _block_bytes)
	{
		u32 id = row->id;

		// If the row identifier indicates it is part of the original message data,
		if (id < _block_count)
		{
			u8 *dest = output_blocks + _block_bytes * id;

			CAT_IF_DUMP(cout << "Copying received row " << id << endl;)

			// If not at the final block,
			if (id != _block_count - 1)
				memcpy(dest, src, _block_bytes);
			else
				memcpy(dest, src, _output_final_bytes);
			copied_rows[id] = 1;
		}
	}
#endif

	// Regenerate any rows that got lost:

	// For each row,
	u8 *dest = output_blocks;
	for (u16 row_i = 0; row_i < _block_count; ++row_i, dest += _block_bytes)
	{
#if defined(CAT_COPY_FIRST_N)
		// If already copied, skip it
		if (copied_rows[row_i])
			continue;
#endif

		CAT_IF_DUMP(cout << "Regenerating row " << row_i << ":";)

		u16 peel_weight, peel_a, peel_x, mix_a, mix_x;
		GeneratePeelRow(row_i, _p_seed, _block_count, _added_count,
			peel_weight, peel_a, peel_x, mix_a, mix_x);

		// Remember first column (there is always at least one)
		u8 *first = _recovery_blocks + _block_bytes * peel_x;

		CAT_IF_DUMP(cout << " " << peel_x;)

		// If peeler has multiple columns,
		if (peel_weight > 1)
		{
			--peel_weight;

			IterateNextColumn(peel_x, _block_count, _block_next_prime, peel_a);

			CAT_IF_DUMP(cout << " " << peel_x;)

			// Combine first two columns into output buffer (faster than memcpy + memxor)
			memxor_set(dest, first, _recovery_blocks + _block_bytes * peel_x, _block_bytes);

			// For each remaining peeler column,
			while (--peel_weight > 0)
			{
				IterateNextColumn(peel_x, _block_count, _block_next_prime, peel_a);

				CAT_IF_DUMP(cout << " " << peel_x;)

				// Mix in each column
				memxor(dest, _recovery_blocks + _block_bytes * peel_x, _block_bytes);
			}

			// Mix first mixer block in directly
			memxor(dest, _recovery_blocks + _block_bytes * (_block_count + mix_x), _block_bytes);
		}
		else
		{
			// Mix first with first mixer block (faster than memcpy + memxor)
			memxor_set(dest, first, _recovery_blocks + _block_bytes * (_block_count + mix_x), _block_bytes);
		}

		CAT_IF_DUMP(cout << " " << (_block_count + mix_x);)

		// Combine remaining two mixer columns together:
		IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
		const u8 *mix0_src = _recovery_blocks + _block_bytes * (_block_count + mix_x);
		CAT_IF_DUMP(cout << " " << (_block_count + mix_x);)

		IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
		const u8 *mix1_src = _recovery_blocks + _block_bytes * (_block_count + mix_x);
		CAT_IF_DUMP(cout << " " << (_block_count + mix_x);)

		memxor_add(dest, mix0_src, mix1_src, _block_bytes);

		CAT_IF_DUMP(cout << endl;)
	} // next row
}


//// Memory Management

Codec::Codec()
{
	// Workspace
	_recovery_blocks = 0;
	_peel_rows = 0;
	_peel_cols = 0;
	_peel_col_refs = 0;
#if defined(CAT_REUSE_COMPRESS)
	_win_table_data = 0;
#endif

	// Matrix
	_ge_compress_matrix = 0;
	_ge_matrix = 0;
	_ge_pivots = 0;

	// Input
	_input_blocks = 0;
	_input_allocated = false;
}

Codec::~Codec()
{
	FreeWorkspace();
	FreeMatrix();
	FreeInput();
}

void Codec::SetInput(const void *message_in)
{
	FreeInput();

	// Set input blocks to the input message
	_input_blocks = (u8*)message_in;
	_input_allocated = false;
}

bool Codec::AllocateInput()
{
	CAT_IF_DUMP(cout << endl << "---- AllocateInput ----" << endl << endl;)

	FreeInput();

	// Allocate input blocks
	_input_allocated = true;
	int input_size = (_block_count + _extra_count) * _block_bytes;
	_input_blocks = new u8[input_size];
	if (!_input_blocks) return false;

	return true;
}

void Codec::FreeInput()
{
	if (_input_allocated && _input_blocks)
	{
		delete []_input_blocks;
		_input_blocks = 0;
	}

	_input_allocated = false;
}

bool Codec::AllocateMatrix()
{
	CAT_IF_DUMP(cout << endl << "---- AllocateMatrix ----" << endl << endl;)

	FreeMatrix();

	// Allocate GE matrix
	const int ge_cols = _defer_count + _added_count;
	const int ge_rows = ge_cols + _extra_count + 1; // One extra for workspace
	const int ge_pitch = (ge_cols + 63) / 64;
	const int ge_matrix_words = ge_rows * ge_pitch;
	_ge_matrix = new u64[ge_matrix_words];
	if (!_ge_matrix) return false;
	_ge_pitch = ge_pitch;
	_ge_rows = ge_cols;

	// Clear entire GE matrix
	memset(_ge_matrix, 0, ge_cols * ge_pitch * sizeof(u64));

	CAT_IF_DUMP(cout << "GE matrix is " << ge_rows << " x " << ge_cols << " with pitch " << ge_pitch << " consuming " << ge_matrix_words * sizeof(u64) << " bytes" << endl;)

	// Allocate GE compress matrix
	const int ge_compress_rows = _block_count;
	const int ge_compress_matrix_words = ge_compress_rows * ge_pitch;
	_ge_compress_matrix = new u64[ge_compress_matrix_words];
	if (!_ge_compress_matrix) return false;

	// Clear entire GE compress matrix
	memset(_ge_compress_matrix, 0, ge_compress_matrix_words * sizeof(u64));

	CAT_IF_DUMP(cout << "Compress matrix is " << ge_compress_rows << " x " << ge_cols << " with pitch " << ge_pitch << " consuming " << ge_compress_matrix_words * sizeof(u64) << " bytes" << endl;)

	// Allocate the pivots
	const int pivot_count = ge_cols + _extra_count;
	const int pivot_words = pivot_count * 2 + ge_cols;
	_ge_pivots = new u16[pivot_words];
	if (!_ge_pivots) return false;
	_ge_row_map = _ge_pivots + pivot_count;
	_ge_col_map = _ge_row_map + pivot_count;

	CAT_IF_DUMP(cout << "Allocated " << pivot_count << " pivots, consuming " << pivot_words*2 << " bytes" << endl;)

	return true;
}

void Codec::FreeMatrix()
{
	if (_ge_matrix)
	{
		delete []_ge_matrix;
		_ge_matrix= 0;
	}

	if (_ge_compress_matrix)
	{
		delete []_ge_compress_matrix;
		_ge_compress_matrix = 0;
	}

	if (_ge_pivots)
	{
		delete []_ge_pivots;
		_ge_pivots = 0;
	}
}

bool Codec::AllocateWorkspace()
{
	CAT_IF_DUMP(cout << endl << "---- AllocateWorkspace ----" << endl << endl;)

	FreeWorkspace();

	// Allocate check blocks
	int check_size = (_block_count + _added_count + 1) * _block_bytes; // +1 for temporary space
	_recovery_blocks = new u8[check_size];
	if (!_recovery_blocks) return false;

	// Allocate space for row data
	_peel_rows = new PeelRow[_block_count + _extra_count];
	if (!_peel_rows) return false;

	// Allocate space for column data
	_peel_cols = new PeelColumn[_block_count];
	if (!_peel_cols) return false;

	// Allocate space for column refs
	_peel_col_refs = new PeelRefs[_block_count];
	if (!_peel_col_refs) return false;

	CAT_IF_DUMP(cout << "Memory overhead for workspace = " <<
		check_size + sizeof(PeelRow) * (_block_count + _extra_count)
		+ sizeof(PeelColumn) * _block_count + sizeof(PeelRefs) * _block_count << " bytes" << endl;)

	// Initialize columns
	for (int ii = 0; ii < _block_count; ++ii)
	{
		_peel_col_refs[ii].row_count = 0;
		_peel_cols[ii].w2_refs = 0;
		_peel_cols[ii].mark = MARK_TODO;
	}

	return true;
}

void Codec::FreeWorkspace()
{
	if (_recovery_blocks)
	{
		delete []_recovery_blocks;
		_recovery_blocks = 0;
	}

	if (_peel_rows)
	{
		delete []_peel_rows;
		_peel_rows = 0;
	}

	if (_peel_cols)
	{
		delete []_peel_cols;
		_peel_cols = 0;
	}

	if (_peel_col_refs)
	{
		delete []_peel_col_refs;
		_peel_col_refs = 0;
	}

#if defined(CAT_REUSE_COMPRESS)
	if (_win_table_data)
	{
		delete []_win_table_data;
		_win_table_data = 0;
	}
#endif
}


//// Diagnostic

#if defined(CAT_DUMP_CODEC_DEBUG) || defined(CAT_DUMP_GE_MATRIX)

void Codec::PrintGEMatrix()
{
	int rows = _ge_rows;
	int cols = _defer_count + _added_count;

	cout << endl << "GE matrix is " << rows << " x " << cols << ":" << endl;

	for (int ii = 0; ii < rows; ++ii)
	{
		for (int jj = 0; jj < cols; ++jj)
		{
			if (_ge_matrix[_ge_pitch * ii + (jj >> 6)] & ((u64)1 << (jj & 63)))
				cout << '1';
			else
				cout << '0';
		}
		cout << endl;
	}

	cout << endl;
}

void Codec::PrintCompressMatrix()
{
	int rows = _block_count;
	int cols = _defer_count + _added_count;

	cout << endl << "Compress matrix is " << rows << " x " << cols << ":" << endl;

	for (int ii = 0; ii < rows; ++ii)
	{
		for (int jj = 0; jj < cols; ++jj)
		{
			if (_ge_compress_matrix[_ge_pitch * ii + (jj >> 6)] & ((u64)1 << (jj & 63)))
				cout << '1';
			else
				cout << '0';
		}
		cout << endl;
	}

	cout << endl;
}

void Codec::PrintPeeled()
{
	cout << "Peeled elements :";

	u16 row_i = _peel_head_rows;
	while (row_i != LIST_TERM)
	{
		PeelRow *row = &_peel_rows[row_i];

		cout << " " << row_i << "x" << row->peel_column;

		row_i = row->next;
	}

	cout << endl;
}

void Codec::PrintDeferredRows()
{
	cout << "Deferred rows :";

	u16 row_i = _defer_head_rows;
	while (row_i != LIST_TERM)
	{
		PeelRow *row = &_peel_rows[row_i];

		cout << " " << row_i;

		row_i = row->next;
	}

	cout << endl;
}

void Codec::PrintDeferredColumns()
{
	cout << "Deferred columns :";

	u16 column_i = _defer_head_columns;
	while (column_i != LIST_TERM)
	{
		PeelColumn *column = &_peel_cols[column_i];

		cout << " " << column_i;

		column_i = column->next;
	}

	cout << endl;
}

#endif // CAT_DUMP_CODEC_DEBUG


//// Setup

Result Codec::InitializeEncoder(int message_bytes, int block_bytes)
{
	Result r = ChooseMatrix(message_bytes, block_bytes);
	if (r == R_WIN)
	{
		// Calculate partial final bytes
		u32 partial_final_bytes = message_bytes % _block_bytes;
		if (partial_final_bytes <= 0) partial_final_bytes = _block_bytes;

		// Encoder-specific
		_input_final_bytes = partial_final_bytes;                                               
		_extra_count = 0;

		if (!AllocateWorkspace())
			return R_OUT_OF_MEMORY;
	}

	return r;
}

Result Codec::InitializeDecoder(int message_bytes, int block_bytes)
{
	Result r = ChooseMatrix(message_bytes, block_bytes);
	if (r == R_WIN)
	{
		// Calculate partial final bytes
		u32 partial_final_bytes = message_bytes % _block_bytes;
		if (partial_final_bytes <= 0) partial_final_bytes = _block_bytes;

		// Decoder-specific
		_used_count = 0;
		_output_final_bytes = partial_final_bytes;
		_input_final_bytes = _block_bytes;
		_extra_count = CAT_MAX_EXTRA_ROWS;

		if (!AllocateInput() || !AllocateWorkspace())
			return R_OUT_OF_MEMORY;
	}

	return r;
}


//// Decoder

Result Codec::DecodeFeed(u32 id, const void *block_in)
{
	// If less than N rows stored,
	u16 row_i = _used_count;
	if (row_i < _block_count)
	{
		// If opportunistic peeling did not fail,
		if (OpportunisticPeeling(row_i, id))
		{
			// Copy the new row data into the input block area
			memcpy(_input_blocks + _block_bytes * row_i, block_in, _block_bytes);

			// If just acquired N blocks,
			if (++_used_count == _block_count)
			{
				// Attempt to solve the matrix and generate recovery blocks
				Result r = SolveMatrix();
				if (r == R_WIN) GenerateRecoveryBlocks();
				return r;
			}
		} // end if opportunistic peeling succeeded

		return R_MORE_BLOCKS;
	}

	// Resume GE from this row
	if (!ResumeSolveMatrix(id, block_in))
		return R_MORE_BLOCKS;

	// Success!  Now generate the output blocks
	GenerateRecoveryBlocks();
	return R_WIN;
}


//// Encoder

Result Codec::EncodeFeed(const void *message_in)
{
	SetInput(message_in);

	// For each input row,
	for (u16 id = 0; id < _block_count; ++id)
	{
		if (!OpportunisticPeeling(id, id))
			return R_BAD_INPUT;
	}

	// Solve matrix and generate recovery blocks
	Result r = SolveMatrix();
	if (r == R_WIN) GenerateRecoveryBlocks();

	return r;
}

void Codec::Encode(u32 id, void *block_out)
{
	u8 *block = reinterpret_cast<u8*>( block_out );

#if defined(CAT_COPY_FIRST_N)
	// For the message blocks,
	if (id < _block_count)
	{
		// Until the final block in message blocks,
		const u8 *src = _input_blocks + _block_bytes * id;
		if ((int)id != _block_count - 1)
		{
			// Copy from the original file data
			memcpy(block, src, _block_bytes);
		}
		else
		{
			// For the final block, copy partial block
			memcpy(block, src, _input_final_bytes);

			// Pad with zeroes
			memset(block + _input_final_bytes, 0, _block_bytes - _input_final_bytes);
		}

		return;
	}
#endif // CAT_COPY_FIRST_N

	CAT_IF_DUMP(cout << "Generating row " << id << ":";)

	u16 peel_weight, peel_a, peel_x, mix_a, mix_x;
	GeneratePeelRow(id, _p_seed, _block_count, _added_count,
		peel_weight, peel_a, peel_x, mix_a, mix_x);

	// Remember first column (there is always at least one)
	u8 *first = _recovery_blocks + _block_bytes * peel_x;

	CAT_IF_DUMP(cout << " " << peel_x;)

	// If peeler has multiple columns,
	if (peel_weight > 1)
	{
		--peel_weight;

		IterateNextColumn(peel_x, _block_count, _block_next_prime, peel_a);

		CAT_IF_DUMP(cout << " " << peel_x;)

		// Combine first two columns into output buffer (faster than memcpy + memxor)
		memxor_set(block, first, _recovery_blocks + _block_bytes * peel_x, _block_bytes);

		// For each remaining peeler column,
		while (--peel_weight > 0)
		{
			IterateNextColumn(peel_x, _block_count, _block_next_prime, peel_a);

			CAT_IF_DUMP(cout << " " << peel_x;)

			// Mix in each column
			memxor(block, _recovery_blocks + _block_bytes * peel_x, _block_bytes);
		}

		// Mix first mixer block in directly
		memxor(block, _recovery_blocks + _block_bytes * (_block_count + mix_x), _block_bytes);
	}
	else
	{
		// Mix first with first mixer block (faster than memcpy + memxor)
		memxor_set(block, first, _recovery_blocks + _block_bytes * (_block_count + mix_x), _block_bytes);
	}

	CAT_IF_DUMP(cout << " " << (_block_count + mix_x);)

	// For each remaining mixer column,
	IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
	memxor(block, _recovery_blocks + _block_bytes * (_block_count + mix_x), _block_bytes);
	CAT_IF_DUMP(cout << " " << (_block_count + mix_x);)

	IterateNextColumn(mix_x, _added_count, _added_next_prime, mix_a);
	memxor(block, _recovery_blocks + _block_bytes * (_block_count + mix_x), _block_bytes);
	CAT_IF_DUMP(cout << " " << (_block_count + mix_x);)

	CAT_IF_DUMP(cout << endl;)
}
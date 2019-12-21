/**
 * Implementation of the Lyra2 Password Hashing Scheme (PHS).
 *
 * Author: The Lyra PHC team (http://www.lyra-kdf.net/) -- 2014.
 *
 * This software is hereby placed in the public domain.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ''AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mm_malloc.h>
#include "compat.h"
#include "lyra2.h"
#include "sponge.h"

//  LYRA2RE 8 cols 8 rows used by lyea2re, allium, phi2, x22i, x25x.
//
//  LYRA2REV2 4 cols 4 rows used by lyra2rev2.
//
//  LYRA2REV3 4 cols 4 rows with an extra twist in calculating
//  rowa in the wandering phase. Used by lyra2rev3.
// 
//  LYRA2Z various cols & rows and supports 80 input. Used by lyra2z,
//  lyra2z330, lyra2h, 


#if defined(__AVX512F__) && defined(__AVX512VL__) && defined(__AVX512DQ__) && defined(__AVX512BW__)

/**
 * Executes Lyra2 based on the G function from Blake2b. This version supports salts and passwords
 * whose combined length is smaller than the size of the memory matrix, (i.e., (nRows x nCols x b) bits,
 * where "b" is the underlying sponge's bitrate). In this implementation, the "basil" is composed by all
 * integer parameters (treated as type "unsigned int") in the order they are provided, plus the value
 * of nCols, (i.e., basil = kLen || pwdlen || saltlen || timeCost || nRows || nCols).
 *
 * @param K The derived key to be output by the algorithm
 * @param kLen Desired key length
 * @param pwd User password
 * @param pwdlen Password length
 * @param salt Salt
 * @param saltlen Salt length
 * @param timeCost Parameter to determine the processing time (T)
 * @param nRows Number or rows of the memory matrix (R)
 * @param nCols Number of columns of the memory matrix (C)
 *
 * @return 0 if the key is generated correctly; -1 if there is an error (usually due to lack of memory for allocation)
 */

// For lyra2rev3.
// convert a simple offset to an index into interleaved data.
// good for state and 4 row matrix. 
// index = ( int( off / 4 ) * 2 ) + ( off mod 4 )

#define offset_to_index( o ) \
   ( ( ( (uint64_t)( (o) & 0xf) / 4 ) * 8 ) + ( (o) % 4 ) )


int LYRA2REV2_2WAY( uint64_t* wholeMatrix, void *K, uint64_t kLen,
             const void *pwd, const uint64_t pwdlen, const uint64_t timeCost,
             const uint64_t nRows, const uint64_t nCols )
{
   //====================== Basic variables ============================//
   uint64_t _ALIGN(256) state[32];
   int64_t row = 2;
   int64_t prev = 1;
   int64_t rowa0 = 0;
   int64_t rowa1 = 0;
   int64_t tau; 
   int64_t step = 1;
   int64_t window = 2;
   int64_t gap = 1;
   //====================================================================/

   const int64_t ROW_LEN_INT64 = BLOCK_LEN_INT64 * nCols;

   // for Lyra2REv2, nCols = 4, v1 was using 8
   const int64_t BLOCK_LEN = (nCols == 4) ? BLOCK_LEN_BLAKE2_SAFE_INT64
                                          : BLOCK_LEN_BLAKE2_SAFE_BYTES;
   uint64_t *ptrWord = wholeMatrix;

   int64_t nBlocksInput = ( ( pwdlen + pwdlen + 6 * sizeof(uint64_t) )
                              / BLOCK_LEN_BLAKE2_SAFE_BYTES ) + 1;

   uint64_t *ptr = wholeMatrix;
   uint64_t *pw = (uint64_t*)pwd;

   memcpy( ptr, pw, 2*pwdlen ); // password 
   ptr += pwdlen>>2;
   memcpy( ptr, pw, 2*pwdlen ); // password lane 1
   ptr += pwdlen>>2;

   // now build the rest interleaving on the fly.

   ptr[0] = ptr[ 4] = kLen;
   ptr[1] = ptr[ 5] = pwdlen;
   ptr[2] = ptr[ 6] = pwdlen;   // saltlen
   ptr[3] = ptr[ 7] = timeCost;
   ptr[8] = ptr[12] = nRows;
   ptr[9] = ptr[13] = nCols;
   ptr[10] = ptr[14] = 0x80;
   ptr[11] = ptr[15] = 0x0100000000000000;

   ptrWord = wholeMatrix;

   absorbBlockBlake2Safe_2way( state, ptrWord, nBlocksInput, BLOCK_LEN );

   //Initializes M[0] and M[1]
   reducedSqueezeRow0_2way( state, &wholeMatrix[0], nCols );

   reducedDuplexRow1_2way( state, &wholeMatrix[0],
                           &wholeMatrix[ 2 * ROW_LEN_INT64 ],  nCols );

   do
   {
     //M[row] = rand; //M[row*] = M[row*] XOR rotW(rand)

     reducedDuplexRowSetup_2way( state, &wholeMatrix[ 2* prev * ROW_LEN_INT64],
                                        &wholeMatrix[ 2* rowa0 * ROW_LEN_INT64],
                                        &wholeMatrix[ 2* row*ROW_LEN_INT64],
                                        nCols );

     rowa0 = (rowa0 + step) & (window - 1);

     prev = row;
     row++;

     if ( rowa0 == 0 )
     {
        step = window + gap;
        window *= 2; 
        gap = -gap;
     }
   } while ( row < nRows );

   //===================== Wandering Phase =============================//
   row = 0;
   for ( tau = 1; tau <= timeCost; tau++ )
   {
      step = ( (tau & 1) == 0 ) ? -1 : ( nRows >> 1 ) - 1;
      do
      {
        rowa0 = state[ 0 ] & (unsigned int)(nRows-1);
        rowa1 = state[ 4 ] & (unsigned int)(nRows-1);

        reducedDuplexRow_2way( state, &wholeMatrix[ 2* prev * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* rowa0 * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* rowa1 * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* row *ROW_LEN_INT64 ],
                                      nCols );
         prev = row;

         row = (row + step) & (unsigned int)(nRows-1); //(USE THIS IF nRows IS A POWER OF 2)

      } while (row != 0);
   }

   //===================== Wrap-up Phase ===============================//
   //Absorbs the last block of the memory matrix
   absorbBlock_2way( state, &wholeMatrix[ 2 * rowa0 *ROW_LEN_INT64 ],
                            &wholeMatrix[ 2 * rowa1 *ROW_LEN_INT64 ] );
   //Squeezes the key
   squeeze_2way( state, K, (unsigned int) kLen );

   return 0;
}

// This version is currently only used by REv3 and has some hard coding
// specific to v3 such as input data size of 32 bytes.
//
// Similarly with REv2. Thedifference with REv3 isn't clear and maybe
// they can be merged.
//
// RE is used by RE, allium. The main difference between RE and REv2
// in the matrix size.
//
// Z also needs to support 80 byte input as well as 32 byte, and odd
// matrix sizes like 330 rows. It is used by lyra2z330, lyra2z, lyra2h.


/////////////////////////////////////////////////

// 2 way 256
// drop salt, salt len arguments, hard code some others.
// Data is interleaved 2x256.

int LYRA2REV3_2WAY( uint64_t* wholeMatrix, void *K, uint64_t kLen,
                    const void *pwd, uint64_t pwdlen, uint64_t timeCost,
                    uint64_t nRows, uint64_t nCols )

// hard coded for 32 byte input as well as matrix size.
// Other required versions include 80 byte input and different block
// sizez

//int LYRA2REV3_2WAY( uint64_t* wholeMatrix, void *K, uint64_t kLen,
//      const void *pwd, const uint64_t pwdlen, const void *salt,
//      const uint64_t saltlen, const uint64_t timeCost, const uint64_t nRows,
//      const uint64_t nCols )
{
   //====================== Basic variables ============================//
   uint64_t _ALIGN(256) state[32];
   int64_t row = 2; 
   int64_t prev = 1;
   int64_t rowa0 = 0;
   int64_t rowa1 = 0;
   int64_t tau; 
   int64_t step = 1;
   int64_t window = 2;
   int64_t gap = 1; 
   uint64_t instance0 = 0;
   uint64_t instance1 = 0;
   //====================================================================/

   const int64_t ROW_LEN_INT64 = BLOCK_LEN_INT64 * nCols;
   const int64_t BLOCK_LEN = BLOCK_LEN_BLAKE2_SAFE_INT64;

   uint64_t *ptrWord = wholeMatrix;

//  2 way 256 rewrite. Salt always == password, and data is interleaved,
//  need to build in parallel as pw isalready interleaved.

   
//  {   password,    (64 or 80 bytes)
//      salt,        (64 or 80 bytes) =  same as password
//      Klen,        (u64)  = 32 bytes
//      pwdlen,      (u64)
//      saltlen,     (u64)
//      timecost,    (u64)
//      nrows,       (u64)
//      ncols,       (u64)
//      0x80,        (byte)
//      { 0 .. 0 },
//      1            (byte)
//   }
   
// input is usually 32 maybe 64, both are aligned to 256 bit vector.
// 80 byte inpput is not aligned complicating matters for lyra2z.   

   int64_t nBlocksInput = ( ( pwdlen + pwdlen + 6 * sizeof(uint64_t) )
                              / BLOCK_LEN_BLAKE2_SAFE_BYTES ) + 1;
   
   uint64_t *ptr = wholeMatrix;
   uint64_t *pw = (uint64_t*)pwd;

   memcpy( ptr, pw, 2*pwdlen ); // password 
   ptr += pwdlen>>2;
   memcpy( ptr, pw, 2*pwdlen ); // password lane 1
   ptr += pwdlen>>2;
 
   // now build the rest interleaving on the fly.

   ptr[0] = ptr[ 4] = kLen;
   ptr[1] = ptr[ 5] = pwdlen;
   ptr[2] = ptr[ 6] = pwdlen;   // saltlen
   ptr[3] = ptr[ 7] = timeCost;
   ptr[8] = ptr[12] = nRows;
   ptr[9] = ptr[13] = nCols;
   ptr[10] = ptr[14] = 0x80;
   ptr[11] = ptr[15] = 0x0100000000000000;

   ptrWord = wholeMatrix;

   absorbBlockBlake2Safe_2way( state, ptrWord, nBlocksInput, BLOCK_LEN );

   reducedSqueezeRow0_2way( state, &wholeMatrix[0], nCols );

   reducedDuplexRow1_2way( state, &wholeMatrix[0],
                           &wholeMatrix[2*ROW_LEN_INT64],  nCols );

   do
   {

      reducedDuplexRowSetup_2way( state, &wholeMatrix[ 2* prev*ROW_LEN_INT64 ],
                                         &wholeMatrix[ 2* rowa0*ROW_LEN_INT64 ],
                                         &wholeMatrix[ 2* row*ROW_LEN_INT64 ],
                                         nCols );

      rowa0 = (rowa0 + step) & (window - 1);

      prev = row;
      row++;

      if (rowa0 == 0)
      {
         step = window + gap; //changes the step: approximately doubles its value
         window *= 2; //doubles the size of the re-visitation window
         gap = -gap; //inverts the modifier to the step
      }

   } while (row < nRows);

   row = 0;
   for (tau = 1; tau <= timeCost; tau++)
   {
      step = ( (tau & 1) == 0 ) ? -1 : ( nRows >> 1 ) - 1;
      do
      {
        instance0 = state[ offset_to_index( instance0 ) ];
        instance1 = (&state[4])[ offset_to_index( instance1 ) ];

        rowa0 = state[ offset_to_index( instance0 )  ]
                & (unsigned int)(nRows-1);
        rowa1 = (state+4)[ offset_to_index( instance1 ) ]
                & (unsigned int)(nRows-1);

        reducedDuplexRow_2way( state, &wholeMatrix[ 2* prev * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* rowa0 * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* rowa1 * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* row*ROW_LEN_INT64 ],
                                      nCols );

        prev = row;
        row = (row + step) & (unsigned int)(nRows-1); 

       } while ( row != 0 );
   }

   absorbBlock_2way( state, &wholeMatrix[2*rowa0*ROW_LEN_INT64],
                            &wholeMatrix[2*rowa1*ROW_LEN_INT64] );

   squeeze_2way( state, K, (unsigned int) kLen );

   return 0;
}

#endif // AVX512

#if 0

//////////////////////////////////////////////////
int LYRA2Z( uint64_t* wholeMatrix, void *K, uint64_t kLen, const void *pwd,
            const uint64_t pwdlen, const void *salt, const uint64_t saltlen,
            const uint64_t timeCost, const uint64_t nRows,
            const uint64_t nCols )
{
    //========================== Basic variables ============================//
    uint64_t _ALIGN(256) state[16];
    int64_t row = 2; //index of row to be processed
    int64_t prev = 1; //index of prev (last row ever computed/modified)
    int64_t rowa = 0; //index of row* (a previous row, deterministically picked during Setup and randomly picked while Wandering)
    int64_t tau; //Time Loop iterator
    int64_t step = 1; //Visitation step (used during Setup and Wandering phases)
    int64_t window = 2; //Visitation window (used to define which rows can be revisited during Setup)
    int64_t gap = 1; //Modifier to the step, assuming the values 1 or -1
//    int64_t i; //auxiliary iteration counter
    //=======================================================================/

    //======= Initializing the Memory Matrix and pointers to it =============//
    //Tries to allocate enough space for the whole memory matrix

    const int64_t ROW_LEN_INT64 = BLOCK_LEN_INT64 * nCols;
//    const int64_t ROW_LEN_BYTES = ROW_LEN_INT64 * 8;

//    memset( wholeMatrix, 0, ROW_LEN_BYTES * nRows );

    //==== Getting the password + salt + basil padded with 10*1 ============//
    //OBS.:The memory matrix will temporarily hold the password: not for saving memory,
    //but this ensures that the password copied locally will be overwritten as soon as possible

    //First, we clean enough blocks for the password, salt, basil and padding
    uint64_t nBlocksInput = ( ( saltlen + pwdlen + 6 *
                       sizeof (uint64_t) ) / BLOCK_LEN_BLAKE2_SAFE_BYTES ) + 1;
    byte *ptrByte = (byte*) wholeMatrix;
    memset( ptrByte, 0, nBlocksInput * BLOCK_LEN_BLAKE2_SAFE_BYTES );

    //Prepends the password
    memcpy(ptrByte, pwd, pwdlen);
    ptrByte += pwdlen;

    //Concatenates the salt
    memcpy(ptrByte, salt, saltlen);
    ptrByte += saltlen;
    //Concatenates the basil: every integer passed as parameter, in the order they are provided by the interface
    memcpy(ptrByte, &kLen, sizeof (uint64_t));
    ptrByte += sizeof (uint64_t);
    memcpy(ptrByte, &pwdlen, sizeof (uint64_t));
    ptrByte += sizeof (uint64_t);
    memcpy(ptrByte, &saltlen, sizeof (uint64_t));
    ptrByte += sizeof (uint64_t);
    memcpy(ptrByte, &timeCost, sizeof (uint64_t));
    ptrByte += sizeof (uint64_t);
    memcpy(ptrByte, &nRows, sizeof (uint64_t));
    ptrByte += sizeof (uint64_t);
    memcpy(ptrByte, &nCols, sizeof (uint64_t));
    ptrByte += sizeof (uint64_t);

    //Now comes the padding
    *ptrByte = 0x80; //first byte of padding: right after the password
    ptrByte = (byte*) wholeMatrix; //resets the pointer to the start of the memory matrix
    ptrByte += nBlocksInput * BLOCK_LEN_BLAKE2_SAFE_BYTES - 1; //sets the pointer to the correct position: end of incomplete block
    *ptrByte ^= 0x01; //last byte of padding: at the end of the last incomplete block

    //=================== Initializing the Sponge State ====================//
    //Sponge state: 16 uint64_t, BLOCK_LEN_INT64 words of them for the bitrate (b) and the remainder for the capacity (c)
//        uint64_t *state = _mm_malloc(16 * sizeof(uint64_t), 32);
//        if (state == NULL) {
//                return -1;
//        }
//    initState( state );

    //============================== Setup Phase =============================//
    //Absorbing salt, password and basil: this is the only place in which the block length is hard-coded to 512 bits
    uint64_t *ptrWord = wholeMatrix;

    absorbBlockBlake2Safe( state, ptrWord, nBlocksInput,
                           BLOCK_LEN_BLAKE2_SAFE_INT64 );
/*
    for ( i = 0; i < nBlocksInput; i++ )
    {
      absorbBlockBlake2Safe( state, ptrWord ); //absorbs each block of pad(pwd || salt || basil)
      ptrWord += BLOCK_LEN_BLAKE2_SAFE_INT64; //goes to next block of pad(pwd || salt || basil)
    }
*/
    //Initializes M[0] and M[1]
        reducedSqueezeRow0(state, &wholeMatrix[0], nCols); //The locally copied password is most likely overwritten here
        reducedDuplexRow1(state, &wholeMatrix[0], &wholeMatrix[ROW_LEN_INT64], nCols);

        do {
                //M[row] = rand; //M[row*] = M[row*] XOR rotW(rand)
                reducedDuplexRowSetup(state, &wholeMatrix[prev*ROW_LEN_INT64], &wholeMatrix[rowa*ROW_LEN_INT64], &wholeMatrix[row*ROW_LEN_INT64], nCols);

                //updates the value of row* (deterministically picked during Setup))
                rowa = (rowa + step) & (window - 1);
                //update prev: it now points to the last row ever computed
                prev = row;
                //updates row: goes to the next row to be computed
                row++;

                //Checks if all rows in the window where visited.
                if (rowa == 0) {
                        step = window + gap; //changes the step: approximately doubles its value
                        window *= 2; //doubles the size of the re-visitation window
                        gap = -gap; //inverts the modifier to the step
                }

        } while (row < nRows);

    //======================== Wandering Phase =============================//
    row = 0; //Resets the visitation to the first row of the memory matrix
    for ( tau = 1; tau <= timeCost; tau++ )
    {
        //Step is approximately half the number of all rows of the memory matrix for an odd tau; otherwise, it is -1
        step = (tau % 2 == 0) ? -1 : nRows / 2 - 1;
        do {
        //Selects a pseudorandom index row*
        //----------------------------------------------------------------------
        //rowa = ((unsigned int)state[0]) & (nRows-1);  //(USE THIS IF nRows IS A POWER OF 2)
        rowa = ((uint64_t) (state[0])) % nRows; //(USE THIS FOR THE "GENERIC" CASE)
        //-----------------------------------------------------------------

        //Performs a reduced-round duplexing operation over M[row*] XOR M[prev], updating both M[row*] and M[row]
                reducedDuplexRow(state, &wholeMatrix[prev*ROW_LEN_INT64], &wholeMatrix[rowa*ROW_LEN_INT64], &wholeMatrix[row*ROW_LEN_INT64], nCols);

        //update prev: it now points to the last row ever computed
        prev = row;

        //updates row: goes to the next row to be computed
        //---------------------------------------------------------------
        //row = (row + step) & (nRows-1);       //(USE THIS IF nRows IS A POWER OF 2)
        row = (row + step) % nRows; //(USE THIS FOR THE "GENERIC" CASE)
        //--------------------------------------------------------------------

      } while (row != 0);
    }

    //========================= Wrap-up Phase ===============================//
    //Absorbs the last block of the memory matrix
    absorbBlock(state, &wholeMatrix[rowa*ROW_LEN_INT64]);

    //Squeezes the key
    squeeze( state, K, kLen );

    return 0;
}

#endif

#if defined(__AVX512F__) && defined(__AVX512VL__) && defined(__AVX512DQ__) && defined(__AVX512BW__)

// Lyra2RE doesn't like the new wholeMatrix implementation
int LYRA2RE_2WAY( void *K, uint64_t kLen, const void *pwd,
                  const uint64_t pwdlen, const uint64_t timeCost,
                  const uint64_t nRows, const uint64_t nCols )
{
   //====================== Basic variables ============================//
   uint64_t _ALIGN(256) state[16];
   int64_t row = 2; //index of row to be processed
   int64_t prev = 1; //index of prev (last row ever computed/modified)
   int64_t rowa0 = 0;
   int64_t rowa1 = 0;
   int64_t tau; //Time Loop iterator
   int64_t step = 1; //Visitation step (used during Setup and Wandering phases)
   int64_t window = 2; //Visitation window (used to define which rows can be revisited during Setup)
   int64_t gap = 1; //Modifier to the step, assuming the values 1 or -1
   int64_t i; //auxiliary iteration counter
   //====================================================================/

   //=== Initializing the Memory Matrix and pointers to it =============//
   //Tries to allocate enough space for the whole memory matrix

   const int64_t ROW_LEN_INT64 = BLOCK_LEN_INT64 * nCols;
   const int64_t ROW_LEN_BYTES = ROW_LEN_INT64 * 8;
   // for Lyra2REv2, nCols = 4, v1 was using 8
   const int64_t BLOCK_LEN = (nCols == 4) ? BLOCK_LEN_BLAKE2_SAFE_INT64
                                          : BLOCK_LEN_BLAKE2_SAFE_BYTES;

   i = (int64_t)ROW_LEN_BYTES * nRows;
   uint64_t *wholeMatrix = _mm_malloc( i, 64 );
   if (wholeMatrix == NULL)
      return -1;

#if defined(__AVX2__)
   memset_zero_256( (__m256i*)wholeMatrix, i>>5 );
#elif defined(__SSE2__)
   memset_zero_128( (__m128i*)wholeMatrix, i>>4 );   
#else
   memset( wholeMatrix, 0, i );
#endif

   uint64_t *ptrWord = wholeMatrix;
   uint64_t *pw = (uint64_t*)pwd;

   //=== Getting the password + salt + basil padded with 10*1 ==========//
   //OBS.:The memory matrix will temporarily hold the password: not for saving memory,
   //but this ensures that the password copied locally will be overwritten as soon as possible

   //First, we clean enough blocks for the password, salt, basil and padding
   int64_t nBlocksInput = ( ( pwdlen + pwdlen + 6 * sizeof(uint64_t) )
                              / BLOCK_LEN_BLAKE2_SAFE_BYTES ) + 1;

   uint64_t *ptr = wholeMatrix;

   memcpy( ptr, pw, 2*pwdlen ); // password 
   ptr += pwdlen>>2;
   memcpy( ptr, pw, 2*pwdlen ); // password lane 1
   ptr += pwdlen>>2;

   // now build the rest interleaving on the fly.

   ptr[0] = ptr[ 4] = kLen;
   ptr[1] = ptr[ 5] = pwdlen;
   ptr[2] = ptr[ 6] = pwdlen;   // saltlen
   ptr[3] = ptr[ 7] = timeCost;
   ptr[8] = ptr[12] = nRows;
   ptr[9] = ptr[13] = nCols;
   ptr[10] = ptr[14] = 0x80;
   ptr[11] = ptr[15] = 0x0100000000000000;

   
/*   
   byte *ptrByte = (byte*) wholeMatrix;

   //Prepends the password
   memcpy(ptrByte, pwd, pwdlen);
   ptrByte += pwdlen;

   //Concatenates the salt
   memcpy(ptrByte, salt, saltlen);
   ptrByte += saltlen;

//   memset( ptrByte, 0, nBlocksInput * BLOCK_LEN_BLAKE2_SAFE_BYTES
//                       - (saltlen + pwdlen) );

   //Concatenates the basil: every integer passed as parameter, in the order they are provided by the interface
   memcpy(ptrByte, &kLen, sizeof(int64_t));
   ptrByte += sizeof(uint64_t);
   v64 = pwdlen;
   memcpy(ptrByte, &v64, sizeof(int64_t));
   ptrByte += sizeof(uint64_t);
   v64 = saltlen;
   memcpy(ptrByte, &v64, sizeof(int64_t));
   ptrByte += sizeof(uint64_t);
   v64 = timeCost;
   memcpy(ptrByte, &v64, sizeof(int64_t));
   ptrByte += sizeof(uint64_t);
   v64 = nRows;
   memcpy(ptrByte, &v64, sizeof(int64_t));
   ptrByte += sizeof(uint64_t);
   v64 = nCols;
   memcpy(ptrByte, &v64, sizeof(int64_t));
   ptrByte += sizeof(uint64_t);

   //Now comes the padding
   *ptrByte = 0x80; //first byte of padding: right after the password
   ptrByte = (byte*) wholeMatrix; //resets the pointer to the start of the memory matrix
   ptrByte += nBlocksInput * BLOCK_LEN_BLAKE2_SAFE_BYTES - 1; //sets the pointer to the correct position: end of incomplete block
   *ptrByte ^= 0x01; //last byte of padding: at the end of the last incomplete block

   //================= Initializing the Sponge State ====================//
   //Sponge state: 16 uint64_t, BLOCK_LEN_INT64 words of them for the bitrate (b) and the remainder for the capacity (c)

//   initState( state );

   //========================= Setup Phase =============================//
   //Absorbing salt, password and basil: this is the only place in which the block length is hard-coded to 512 bits

   ptrWord = wholeMatrix;

*/

   absorbBlockBlake2Safe_2way( state, ptrWord, nBlocksInput, BLOCK_LEN );
/*
   for (i = 0; i < nBlocksInput; i++)
   {
       absorbBlockBlake2Safe( state, ptrWord ); //absorbs each block of pad(pwd || salt || basil)
       ptrWord += BLOCK_LEN; //goes to next block of pad(pwd || salt || basil)
   }
*/
   //Initializes M[0] and M[1]
   reducedSqueezeRow0_2way( state, &wholeMatrix[0], nCols ); //The locally copied password is most likely overwritten here

   reducedDuplexRow1_2way( state, &wholeMatrix[0],
                                  &wholeMatrix[ 2 * ROW_LEN_INT64], nCols );

   do
   {
      //M[row] = rand; //M[row*] = M[row*] XOR rotW(rand)

      reducedDuplexRowSetup_2way( state, &wholeMatrix[ 2* prev*ROW_LEN_INT64 ],
                                         &wholeMatrix[ 2* rowa0*ROW_LEN_INT64 ],
                                         &wholeMatrix[ 2* row*ROW_LEN_INT64 ],
                                         nCols );

      //updates the value of row* (deterministically picked during Setup))
      rowa0 = (rowa0 + step) & (window - 1);
      //update prev: it now points to the last row ever computed

      prev = row;
      //updates row: goes to the next row to be computed
      row++;

      //Checks if all rows in the window where visited.
      if (rowa0 == 0)
      {
         step = window + gap; //changes the step: approximately doubles its value
         window *= 2; //doubles the size of the re-visitation window
         gap = -gap; //inverts the modifier to the step
      }

   } while (row < nRows);

   //===================== Wandering Phase =============================//
   row = 0; //Resets the visitation to the first row of the memory matrix
   for (tau = 1; tau <= timeCost; tau++)
   {
      step = ((tau & 1) == 0) ? -1 : (nRows >> 1) - 1;
      do
      {
        rowa0 = state[ 0 ] & (unsigned int)(nRows-1);
        rowa1 = state[ 4 ] & (unsigned int)(nRows-1);

        reducedDuplexRow_2way( state, &wholeMatrix[ 2* prev * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* rowa0 * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* rowa1 * ROW_LEN_INT64 ],
                                      &wholeMatrix[ 2* row *ROW_LEN_INT64 ],
                                      nCols );

           //update prev: it now points to the last row ever computed
           prev = row;

           //updates row: goes to the next row to be computed
           //----------------------------------------------------
           row = (row + step) & (unsigned int)(nRows-1); //(USE THIS IF nRows IS A POWER OF 2)
           //row = (row + step) % nRows; //(USE THIS FOR THE "GENERIC" CASE)
           //----------------------------------------------------

       } while (row != 0);
   }

   //===================== Wrap-up Phase ===============================//
   //Absorbs the last block of the memory matrix
   absorbBlock_2way( state, &wholeMatrix[ 2 * rowa0 *ROW_LEN_INT64],
                            &wholeMatrix[ 2 * rowa1 *ROW_LEN_INT64] );
   //Squeezes the key
   squeeze_2way( state, K, (unsigned int) kLen );

   //================== Freeing the memory =============================//
   _mm_free(wholeMatrix);

   return 0;
}

#endif
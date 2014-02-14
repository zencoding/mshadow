#ifndef MSHADOW_TENSOR_EXPR_ENGINE_INL_HPP
#define MSHADOW_TENSOR_EXPR_ENGINE_INL_HPP
/*!
 * \file tensor_expr_engine-inl.hpp
 * \brief definitions of how expressions should be evaluated
 * \author Tianqi Chen, Bing Xu
 */
#include <cublas.h>
#include "tensor_expr.h"

#define USE_MKL 1

#ifdef USE_MKL
#include <mkl.h>
#include <mkl_cblas.h>
#endif

namespace mshadow{
    namespace expr{
        // This part of code gives plan that can be used to carry out execution
        template<typename ExpType>
        class Plan{
        public:
            /*!
             * \brief evaluate the expression at index [y][x]
             *        to be implemented by SubType
             */
            MSHADOW_XINLINE real_t Eval( index_t y, index_t x ) const;
        };

        template <typename Device, int dim>
        class Plan< Tensor<Device,dim> >{
        public:
            Plan( const Tensor<Device,dim> &t )
                :dptr_(t.dptr),stride_(t.shape.stride_){}
            MSHADOW_XINLINE real_t Eval( index_t y, index_t x ) const{
                return dptr_[ y * stride_ + x ];
            }
        private:
            const real_t  *dptr_;
            index_t stride_;
        };

        template<>
        class Plan<ScalarExp>{
        public:
            Plan( real_t scalar ):scalar_(scalar){}
            /*! \brief evaluate at [y][x] */
            MSHADOW_XINLINE real_t Eval( index_t y, index_t x ) const{
                    return scalar_;
            }
        private:
            real_t scalar_;
        };

        template<typename OP, typename TA, typename TB,int etype>
        class Plan< BinaryMapExp<OP,TA,TB,etype> >{
        public:
            Plan( const Plan<TA> &lhs, const Plan<TB> &rhs )
                :lhs_(lhs), rhs_(rhs){}
            MSHADOW_XINLINE real_t Eval( index_t y, index_t x ) const{
                return OP::Map( lhs_.Eval( y, x ), rhs_.Eval( y, x ) );
            }
        private:
            Plan<TA> lhs_;
            Plan<TB> rhs_;
        };

        template<typename OP, typename TA, int etype>
        class Plan< UnaryMapExp<OP,TA,etype> >{
        public:
            Plan( const Plan<TA> &src ):src_(src){}
            MSHADOW_XINLINE real_t Eval( index_t y, index_t x ) const{
                return OP::Map( src_.Eval( y, x ) );
            }
        private:
            Plan<TA> src_;
        };

        // allow UnaryMap see the plan
        template<typename OP, typename TA, typename TB, int etype>
        inline Plan< BinaryMapExp<OP,TA,TB,etype> > MakePlan( const BinaryMapExp<OP,TA,TB,etype> &e );

        // translate from exp to execution plan
        inline Plan<ScalarExp> MakePlan( const ScalarExp &e ){
            return Plan<ScalarExp>( e.scalar_ );
        }

        template<typename T>
        inline Plan<T> MakePlan( const ContainerExp<T> &e ){
            return Plan<T>( e.self() );
        }

        template<typename OP, typename TA, int etype>
        inline Plan< UnaryMapExp<OP,TA,etype> > MakePlan( const UnaryMapExp<OP,TA,etype> &e ){
            return Plan< UnaryMapExp<OP,TA,etype> >( MakePlan(e.src_) );
        }

        template<typename OP, typename TA, typename TB, int etype>
        inline Plan< BinaryMapExp<OP,TA,TB,etype> > MakePlan( const BinaryMapExp<OP,TA,TB,etype> &e ){
                return Plan< BinaryMapExp<OP,TA,TB,etype> >( MakePlan(e.lhs_), MakePlan(e.rhs_) );
        }
    }; // namespace expr

    namespace expr{
        /*!
         * \brief static type check template,
         *        if a expression E does not match type Device, dim, then kPass = dim
         * \tparam Device the type of Device
         * \tparam dim dimension of the tensor
         * \tparam E expression
         */
        template<typename Device, int dim, typename E>
        struct TypeCheck{
            const static bool kPass = false;
        };

        template<typename Device, int dim>
        struct TypeCheck<Device,dim,ScalarExp>{
            const static bool kPass = true;
        };
        template<typename Device, int dim>
        struct TypeCheck<Device,dim, Tensor<Device,dim> >{
            const static bool kPass = true;
        };
        template<typename Device, int dim, typename OP, typename TA, int etype>
        struct TypeCheck<Device,dim, UnaryMapExp<OP,TA,etype> >{
            const static bool kPass = TypeCheck<Device, dim, TA>::kPass;
        };
        template<typename Device, int dim, typename OP, typename TA, typename TB, int etype>
        struct TypeCheck<Device,dim, BinaryMapExp<OP,TA,TB,etype> >{
            const static bool kPass = TypeCheck<Device, dim, TA>::kPass && TypeCheck<Device,dim, TB>::kPass;
        };

        template<bool kPass>
        struct TypeCheckPass;
        template<>
        struct TypeCheckPass<false>{};
        template<>
        struct TypeCheckPass<true>{
            inline static void Error_All_Tensor_in_Exp_Must_Have_Same_Type( void ){}
        };
    }; // namespace expr

    namespace expr{
        // check shape consistency
        template<int dim>
        struct ShapeCheck{
            inline static bool Check( const ScalarExp &exp, const Shape<dim> &shape ){
                return true;
            }
            template<typename Device>
            inline static bool Check( const Tensor<Device,dim> &t, const Shape<dim> &shape ){
                return t.shape == shape;
            }
            template<typename OP, typename TA, int etype>
            inline static bool Check( const UnaryMapExp<OP,TA,etype> &t, const Shape<dim> &shape ){
                return Check( t.src_, shape );
            }
            template<typename OP, typename TA, typename TB, int etype>
            inline static bool Check( const BinaryMapExp<OP,TA,TB,etype> &t, const Shape<dim> &shape ){
                return Check( t.lhs_, shape ) && Check( t.rhs_, shape );
            }
        };
    }; // namespace expr

    namespace expr{
        template<typename Device, int ddim, int ldim, int rdim, bool ltrans, bool rtrans>
        struct DotEngine{
            inline static void Eval( Tensor<Device,ddim> &dst, const Tensor<Device,ldim> &lhs, const Tensor<Device,rdim> &rhs, real_t scale );
        };

        template<typename SV, typename Device, int dim>
        struct ExpEngine<SV, Tensor<Device,dim> >{
            template<typename E>
            inline static void Eval( Tensor<Device,dim>& dst, const Exp<E,type::kMapper> &exp ){
                // static type check and shape check
                TypeCheckPass< TypeCheck<Device,dim,E>::kPass >::Error_All_Tensor_in_Exp_Must_Have_Same_Type();
                utils::Assert( ShapeCheck<dim>::Check( exp.self(), dst.shape ), "shape of Tensors in expression is not consistent with target" );
                MapPlan<SV>( dst, MakePlan( exp.self() ) );

            }
            template<typename E>
            inline static void Eval( Tensor<Device,dim>& dst, const Exp<E,type::kContainer> &exp ){
                // static type check and shape check
                TypeCheckPass< TypeCheck<Device,dim,E>::kPass >::Error_All_Tensor_in_Exp_Must_Have_Same_Type();
                utils::Assert( ShapeCheck<dim>::Check( exp.self(), dst.shape ), "shape of Tensors in expression is not consistent with target" );
                MapPlan<SV>( dst, MakePlan( exp.self() ) );
            }
            template<int ldim,int rdim,bool ltrans,bool rtrans>
            inline static void Eval( Tensor<Device,dim> &dst, const DotExp< Tensor<Device,ldim>, Tensor<Device,rdim>, ltrans, rtrans > &exp ){
                DotEngine<Device,dim,ldim,rdim,ltrans,rtrans>::Eval( dst, exp.lhs_, exp.rhs_, exp.scale_ );
            }
        };

        // evaluating DotExp
        template<bool ltrans, bool rtrans>
        struct DotEngine<cpu,2,2,2,ltrans,rtrans>{
            inline static void Eval( CTensor2D &dst, const CTensor2D &lhs, const CTensor2D &rhs, real_t scale ) {
                CBLAS_TRANSPOSE op_lhs = ltrans ? CblasTrans : CblasNoTrans;
                CBLAS_TRANSPOSE op_rhs = rtrans ? CblasTrans : CblasNoTrans;

                index_t M = ltrans ? lhs.shape[1] : lhs.shape[0]; // op(lhs) row
                index_t K = ltrans ? lhs.shape[0] : lhs.shape[1]; // op(lhs) col
                index_t N = rtrans ? rhs.shape[0] : rhs.shape[1]; // op(rhs) col

                index_t LDA = ltrans ? lhs.shape[1] : lhs.shape[0]; // op(lhs) leading row
                index_t LDB = rtrans ? rhs.shape[1] : rhs.shape[0]; // op(rhs) leading row
                // Sleepy, shape may wrong. Check after sleep.
                // Lazy, let MKL itself crash
                // utils::Assert(, "Matrix Dimension Error");
                cblas_sgemm(CblasColMajor, \
                            op_lhs, op_rhs, \
                            M, N, K, \
                            scale, \
                            lhs.dptr, LDA, \
                            rhs.dptr, LDB, \
                            0, \
                            dst.dptr, LDA); // dst leading row
            }
        };
        template<bool rtrans>
        struct DotEngine<cpu,1,1,2,false,rtrans>{
            inline static void Eval( CTensor1D &dst, const CTensor1D &lhs, const CTensor2D &rhs, real_t scale ){
                // TODO link to MKL dst = dot( lhs, rhs[.T] );
                CBLAS_TRANSPOSE op_rhs = rtrans ? CblasTrans : CblasNoTrans;


                //cblas_sgemv(
            }
        };
        template<>
        struct DotEngine<cpu,2,1,1,true,false>{
            inline static void Eval( CTensor2D &dst, const CTensor1D &lhs, const CTensor1D &rhs, real_t scale ){
                // TODO link to MKL dst = dot( lhs.T, rhs );
                // cblas_sgemv
            }
        };

        template<bool ltrans, bool rtrans>
        struct DotEngine<gpu,2,2,2,ltrans,rtrans>{
            inline static void Eval( GTensor2D &dst, const GTensor2D &lhs, const GTensor2D &rhs, real_t scale ){
                // TODO link to CuBLAS dst = dot( lhs[.T], rhs[.T] );
                // How to deal with cuBLAS handle? static global?
            }
        };
        template<bool rtrans>
        struct DotEngine<gpu,1,1,2,false,rtrans>{
            inline static void Eval( GTensor1D &dst, const GTensor1D &lhs, const GTensor2D &rhs, real_t scale ){
                // TODO link to CuBLAS dst = dot( lhs, rhs[.T] );
            }
        };
        template<>
        struct DotEngine<gpu,2,1,1,true,false>{
            inline static void Eval( GTensor2D &dst, const GTensor1D &lhs, const GTensor1D &rhs, real_t scale ){
                // TODO link to CuBLAS dst = dot( lhs.T, rhs );
            }
        };

    }; // namespace expr

    // implementation of MapExp
    template<typename Saver, typename Device, int dim, typename E, int etype>
    inline void MapExp(Tensor<Device,dim> dst, const expr::Exp<E,etype> &exp ){
        expr::ExpEngine<Saver,Tensor<Device,dim> >::Eval( dst, exp );
    }
};
#endif
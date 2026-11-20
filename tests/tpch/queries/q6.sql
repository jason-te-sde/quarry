-- Derived from TPC-H; not a TPC-H benchmark result.
-- Q6, forecasting revenue change. Validation parameters: date = 1994-01-01, discount = 0.06, quantity = 24.
select
    sum(l_extendedprice * l_discount) as revenue
from
    lineitem
where
    l_shipdate >= date '1994-01-01'
    and l_shipdate < date '1994-01-01' + interval '1' year
    and l_discount between 0.06 - 0.01 and 0.06 + 0.01
    and l_quantity < 24
